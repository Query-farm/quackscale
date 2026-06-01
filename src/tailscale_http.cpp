#include "tailscale_http.hpp"

#include "tailscale_bridge.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/helper.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension_helper.hpp"

#include <cstdlib>
#include <string>

#ifndef _WIN32
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

namespace duckdb {

//===--------------------------------------------------------------------===//
// Host parsing / tailnet detection
//===--------------------------------------------------------------------===//

// Strip scheme, path and (optionally) port from a proto_host_port string.
// IPv4 / hostname only — IPv6 literals are not handled yet.
static void ParseProtoHostPort(const string &proto_host_port, string &host_out, string &port_out) {
	string h = proto_host_port;
	auto scheme = h.find("://");
	if (scheme != string::npos) {
		h = h.substr(scheme + 3);
	}
	auto slash = h.find('/');
	if (slash != string::npos) {
		h = h.substr(0, slash);
	}
	auto colon = h.rfind(':');
	if (colon != string::npos) {
		port_out = h.substr(colon + 1);
		host_out = h.substr(0, colon);
	} else {
		host_out = h;
		port_out = "80";
	}
}

bool IsTailnetHost(const string &proto_host_port) {
	// Only intercept plaintext HTTP (or scheme-less) URLs. We speak plaintext over WireGuard and
	// never negotiate TLS, so https:// URLs — even to a tailnet host — are left to the delegate
	// util (httpfs) which has real TLS, rather than being mis-sent as cleartext.
	if (StringUtil::StartsWith(StringUtil::Lower(proto_host_port), "https://")) {
		return false;
	}
	string host, port;
	ParseProtoHostPort(proto_host_port, host, port);
	if (host.empty()) {
		return false;
	}
	// MagicDNS FQDNs.
	if (StringUtil::EndsWith(StringUtil::Lower(host), ".ts.net")) {
		return true;
	}
	// IPv4 in the CGNAT range 100.64.0.0/10 (100.64.x.x .. 100.127.x.x).
	auto parts = StringUtil::Split(host, ".");
	if (parts.size() != 4) {
		return false;
	}
	for (auto &p : parts) {
		if (p.empty()) {
			return false;
		}
		for (char c : p) {
			if (c < '0' || c > '9') {
				return false;
			}
		}
	}
	auto octet0 = std::atoi(parts[0].c_str());
	auto octet1 = std::atoi(parts[1].c_str());
	return octet0 == 100 && octet1 >= 64 && octet1 <= 127;
}

//===--------------------------------------------------------------------===//
// Plaintext HTTP/1.1 over a tsnet-dialed fd, with keep-alive
//===--------------------------------------------------------------------===//

namespace {

idx_t ParsePort(const string &port) {
	auto value = std::atoi(port.c_str());
	if (value <= 0 || value > 65535) {
		return 80;
	}
	return static_cast<idx_t>(value);
}

// Strict decimal length: true only if `s` is non-empty, all ASCII digits, and in range. Rejects
// "123abc", "+5", "-1" (which would otherwise wrap huge via stoull and drive a runaway read).
bool ParseContentLength(const string &s, size_t &out) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		if (c < '0' || c > '9') {
			return false;
		}
	}
	try {
		out = static_cast<size_t>(std::stoull(s));
	} catch (...) {
		return false;
	}
	return true;
}

// Strict hex chunk size: hex digits only, no "0x" prefix or sign (extensions are stripped before
// this by the caller).
bool ParseChunkSize(const string &s, size_t &out) {
	if (s.empty()) {
		return false;
	}
	for (char c : s) {
		bool hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
		if (!hex) {
			return false;
		}
	}
	try {
		out = static_cast<size_t>(std::stoull(s, nullptr, 16));
	} catch (...) {
		return false;
	}
	return true;
}

#ifndef _WIN32

bool WriteAll(int fd, const char *data, size_t len) {
	size_t off = 0;
	while (off < len) {
		ssize_t w = write(fd, data + off, len - off);
		if (w <= 0) {
			return false;
		}
		off += static_cast<size_t>(w);
	}
	return true;
}

#endif

// Build the request line + headers. Skips any caller-supplied
// Host/Content-Length/Connection/Transfer-Encoding so we own framing. We keep the
// connection alive (HTTP/1.1 default) and frame the response ourselves.
string BuildRequestHead(const string &method, const string &path, const string &host, const string &port,
                        const HTTPHeaders &headers, idx_t content_length, bool has_body) {
	string req = method + " " + (path.empty() ? "/" : path) + " HTTP/1.1\r\n";
	req += "Host: " + host + ":" + port + "\r\n";
	for (auto &h : headers) {
		auto key = StringUtil::Lower(h.first);
		if (key == "host" || key == "content-length" || key == "connection" || key == "transfer-encoding") {
			continue;
		}
		req += h.first + ": " + h.second + "\r\n";
	}
	if (has_body) {
		req += "Content-Length: " + std::to_string(content_length) + "\r\n";
	}
	req += "Connection: keep-alive\r\n\r\n";
	return req;
}

// Parse the status line + header block. Returns the HTTP minor version (0 or 1) so the
// caller can apply the right keep-alive default.
int ParseHead(const string &head, int &status_out, string &reason_out, HTTPHeaders &headers_out) {
	int http_minor = 1;
	auto lines = StringUtil::Split(head, "\r\n");
	if (lines.empty()) {
		status_out = 0;
		return http_minor;
	}
	// Status line: "HTTP/1.1 200 OK"
	auto &status_line = lines[0];
	if (StringUtil::StartsWith(status_line, "HTTP/1.0")) {
		http_minor = 0;
	}
	auto sp1 = status_line.find(' ');
	if (sp1 != string::npos) {
		auto sp2 = status_line.find(' ', sp1 + 1);
		auto code = status_line.substr(sp1 + 1, sp2 == string::npos ? string::npos : sp2 - sp1 - 1);
		status_out = std::atoi(code.c_str());
		if (sp2 != string::npos) {
			reason_out = status_line.substr(sp2 + 1);
		}
	}
	for (idx_t i = 1; i < lines.size(); i++) {
		auto colon = lines[i].find(':');
		if (colon == string::npos) {
			continue;
		}
		auto key = lines[i].substr(0, colon);
		auto value = lines[i].substr(colon + 1);
		StringUtil::Trim(key);
		StringUtil::Trim(value);
		headers_out.Insert(key, value);
	}
	return http_minor;
}

// Lower-cased header value, or "" if the header is absent (never touches a missing key).
string HeaderValueLower(const HTTPHeaders &headers, const string &key) {
	if (!headers.HasHeader(key)) {
		return string();
	}
	return StringUtil::Lower(headers.GetHeaderValue(key));
}

unique_ptr<HTTPResponse> ToHTTPResponse(TailscaleHTTPClient::ParsedResponse &parsed) {
	auto response = make_uniq<HTTPResponse>(HTTPUtil::ToStatusCode(parsed.status));
	response->body = std::move(parsed.body);
	response->reason = std::move(parsed.reason);
	response->headers = std::move(parsed.headers);
	// Reflect the status in success/request_error: HTTPResponse::success defaults true, so without
	// this a 4xx/5xx — or an unparseable status line (status 0) — would look like a successful
	// fetch to a caller using Success()/HasRequestError().
	response->success = parsed.status >= 200 && parsed.status < 400;
	if (!response->success) {
		response->request_error = StringUtil::Format("tailscale HTTP status %d %s", parsed.status, parsed.reason);
	}
	return response;
}

} // namespace

TailscaleHTTPClient::TailscaleHTTPClient(const string &proto_host_port) : HTTPClient(proto_host_port) {
	ParseProtoHostPort(proto_host_port, host, port);
}

TailscaleHTTPClient::~TailscaleHTTPClient() {
	CloseConn();
}

void TailscaleHTTPClient::Initialize(HTTPParams &http_params) {
	if (http_params.timeout > 0) {
		timeout_seconds = http_params.timeout;
	}
}

void TailscaleHTTPClient::CloseConn() {
#ifndef _WIN32
	if (fd >= 0) {
		close(fd);
	}
#endif
	fd = -1;
	rx.clear();
}

bool TailscaleHTTPClient::ReadMore() {
#ifndef _WIN32
	char buf[16384];
	ssize_t n = read(fd, buf, sizeof(buf));
	if (n == 0) {
		return false; // clean EOF (peer closed)
	}
	if (n < 0) {
		read_error = true; // socket error / timeout — NOT a clean end of message
		return false;
	}
	rx.append(buf, static_cast<size_t>(n));
	return true;
#else
	read_error = true;
	return false;
#endif
}

bool TailscaleHTTPClient::ReadChunkedBody(string &body) {
	for (;;) {
		// Chunk-size line.
		size_t eol;
		while ((eol = rx.find("\r\n")) == string::npos) {
			if (!ReadMore()) {
				return false;
			}
		}
		auto size_line = rx.substr(0, eol);
		rx.erase(0, eol + 2);
		auto semi = size_line.find(';'); // strip chunk extensions
		if (semi != string::npos) {
			size_line = size_line.substr(0, semi);
		}
		StringUtil::Trim(size_line);
		size_t chunk_size = 0;
		if (!ParseChunkSize(size_line, chunk_size)) {
			return false; // malformed chunk size — framing is unrecoverable
		}

		if (chunk_size == 0) {
			// Trailer headers (if any), then a blank line terminates the message.
			for (;;) {
				size_t tend;
				while ((tend = rx.find("\r\n")) == string::npos) {
					if (!ReadMore()) {
						return false;
					}
				}
				bool blank = (tend == 0);
				rx.erase(0, tend + 2);
				if (blank) {
					return true;
				}
			}
		}

		while (rx.size() < chunk_size + 2) { // +2 for the chunk's trailing CRLF
			if (!ReadMore()) {
				return false;
			}
		}
		body.append(rx, 0, chunk_size);
		rx.erase(0, chunk_size + 2);
	}
}

bool TailscaleHTTPClient::ReadResponse(const string &method, ParsedResponse &out) {
	read_error = false;
	// 1. Header block.
	size_t header_end;
	while ((header_end = rx.find("\r\n\r\n")) == string::npos) {
		if (!ReadMore()) {
			return false;
		}
	}
	auto head = rx.substr(0, header_end);
	rx.erase(0, header_end + 4); // rx now begins at the body
	int http_minor = ParseHead(head, out.status, out.reason, out.headers);

	// Keep-alive default: HTTP/1.1 reuses unless told to close; HTTP/1.0 closes unless told to keep.
	auto connection = HeaderValueLower(out.headers, "Connection");
	if (http_minor == 0) {
		out.connection_close = (connection != "keep-alive");
	} else {
		out.connection_close = (connection == "close");
	}

	// 2. Body framing. These statuses (and HEAD) never carry a body.
	bool no_body = method == "HEAD" || out.status == 204 || out.status == 304 ||
	               (out.status >= 100 && out.status < 200);
	if (no_body) {
		return true;
	}

	if (HeaderValueLower(out.headers, "Transfer-Encoding").find("chunked") != string::npos) {
		return ReadChunkedBody(out.body);
	}
	if (out.headers.HasHeader("Content-Length")) {
		auto raw = out.headers.GetHeaderValue("Content-Length");
		StringUtil::Trim(raw);
		size_t len = 0;
		if (!ParseContentLength(raw, len)) {
			out.connection_close = true; // malformed length — connection framing is now unknown
			return false;
		}
		while (rx.size() < len) {
			if (!ReadMore()) {
				return false; // short read: surfaced as a request failure, never a truncated body
			}
		}
		out.body = rx.substr(0, len);
		rx.erase(0, len);
		// On a kept-alive connection there must be nothing after the body until our next request;
		// leftover bytes mean the stream is desynced, so don't reuse this connection.
		if (!out.connection_close && !rx.empty()) {
			out.connection_close = true;
		}
		return true;
	}

	// No length signal: the body runs until EOF, so the connection cannot be reused.
	out.connection_close = true;
	while (ReadMore()) {
	}
	if (read_error) {
		return false; // a socket error/timeout mid-body is not a clean EOF — don't accept a truncated body
	}
	out.body = std::move(rx);
	rx.clear();
	return true;
}

TailscaleHTTPClient::ParsedResponse TailscaleHTTPClient::RoundTrip(const string &method, const string &path,
                                                                   const HTTPHeaders &headers, const_data_ptr_t body,
                                                                   idx_t body_len, bool has_body) {
#ifndef _WIN32
	// Try at most twice: a connection reused from the keep-alive pool may have been closed by the
	// peer while idle. A failure on a *fresh* connection is a real error. Only idempotent requests
	// (no body — GET/HEAD/DELETE) are retried; resending a POST/PUT body could double-execute a
	// write the server already processed before dropping the socket.
	for (int attempt = 0; attempt < 2; attempt++) {
		bool reused = (fd >= 0);
		if (fd < 0) {
			fd = TailscaleBridge::Get().DialTCP(host, ParsePort(port));
			rx.clear();
			// Best-effort read/write deadlines so a hung peer cannot block a query forever.
			struct timeval tv;
			tv.tv_sec = static_cast<time_t>(timeout_seconds);
			tv.tv_usec = 0;
			setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
			setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
		}

		auto head = BuildRequestHead(method, path, host, port, headers, body_len, has_body);
		bool ok = WriteAll(fd, head.data(), head.size());
		if (ok && has_body && body_len > 0) {
			ok = WriteAll(fd, const_char_ptr_cast(body), body_len);
		}

		ParsedResponse parsed;
		if (ok) {
			ok = ReadResponse(method, parsed);
		}
		if (!ok) {
			CloseConn();
			if (reused && !has_body) {
				continue; // stale idle keep-alive on an idempotent request: redial and retry once
			}
			throw IOException("tailscale HTTP: request to %s:%s failed", host, port);
		}

		if (parsed.connection_close) {
			CloseConn();
		}
		return parsed;
	}
	throw IOException("tailscale HTTP: request to %s:%s failed after retry", host, port);
#else
	(void)method;
	(void)path;
	(void)headers;
	(void)body;
	(void)body_len;
	(void)has_body;
	throw NotImplementedException("tailscale HTTP routing is not supported on Windows");
#endif
}

unique_ptr<HTTPResponse> TailscaleHTTPClient::Get(GetRequestInfo &info) {
	auto merged = BaseRequest::MergeHeaders(info.headers, info.params);
	auto parsed = RoundTrip("GET", info.path, merged, nullptr, 0, false);
	auto response = ToHTTPResponse(parsed);

	// Honor the streaming handlers httpfs uses for ranged/streamed reads.
	bool keep_going = true;
	if (info.response_handler) {
		keep_going = info.response_handler(*response);
	}
	if (keep_going && info.content_handler && !response->body.empty()) {
		info.content_handler(const_data_ptr_cast(response->body.data()), response->body.size());
	}
	return response;
}

unique_ptr<HTTPResponse> TailscaleHTTPClient::Post(PostRequestInfo &info) {
	auto merged = BaseRequest::MergeHeaders(info.headers, info.params);
	auto parsed = RoundTrip("POST", info.path, merged, info.buffer_in, info.buffer_in_len, true);
	info.buffer_out = parsed.body;
	return ToHTTPResponse(parsed);
}

unique_ptr<HTTPResponse> TailscaleHTTPClient::Put(PutRequestInfo &info) {
	auto merged = BaseRequest::MergeHeaders(info.headers, info.params);
	// PutRequestInfo carries content_type out of band; surface it as a header unless the
	// caller already supplied one.
	if (!info.content_type.empty() && !merged.HasHeader("Content-Type")) {
		merged.Insert("Content-Type", info.content_type);
	}
	auto parsed = RoundTrip("PUT", info.path, merged, info.buffer_in, info.buffer_in_len, true);
	return ToHTTPResponse(parsed);
}

unique_ptr<HTTPResponse> TailscaleHTTPClient::Head(HeadRequestInfo &info) {
	auto merged = BaseRequest::MergeHeaders(info.headers, info.params);
	auto parsed = RoundTrip("HEAD", info.path, merged, nullptr, 0, false);
	return ToHTTPResponse(parsed);
}

unique_ptr<HTTPResponse> TailscaleHTTPClient::Delete(DeleteRequestInfo &info) {
	auto merged = BaseRequest::MergeHeaders(info.headers, info.params);
	auto parsed = RoundTrip("DELETE", info.path, merged, nullptr, 0, false);
	return ToHTTPResponse(parsed);
}

//===--------------------------------------------------------------------===//
// TailscaleHTTPUtil — routing + keep-alive client pool
//===--------------------------------------------------------------------===//

unique_ptr<HTTPClient> TailscaleHTTPUtil::InitializeClient(HTTPParams &http_params, const string &proto_host_port) {
	if (!IsTailnetHost(proto_host_port)) {
		return prev.InitializeClient(http_params, proto_host_port);
	}
	// Reuse an idle keep-alive client for this host if we have one.
	{
		std::lock_guard<std::mutex> guard(pool_mutex);
		auto entry = idle_clients.find(proto_host_port);
		if (entry != idle_clients.end() && !entry->second.empty()) {
			auto client = std::move(entry->second.back());
			entry->second.pop_back();
			client->Initialize(http_params);
			return client;
		}
	}
	auto client = make_uniq<TailscaleHTTPClient>(proto_host_port);
	client->Initialize(http_params);
	return std::move(client);
}

void TailscaleHTTPUtil::CloseClient(unique_ptr<HTTPClient> &&client) {
	if (!client || !IsTailnetHost(client->GetBaseUrl())) {
		// Non-tailnet: let httpfs keep caching its keep-alive clients.
		prev.CloseClient(std::move(client));
		return;
	}
	// Tailnet client: return it to the idle pool (keeping its live fd) up to the cap;
	// drop the overflow. IsTailnetHost(base_url) == true ⇒ we created a TailscaleHTTPClient.
	std::lock_guard<std::mutex> guard(pool_mutex);
	auto &bucket = idle_clients[client->GetBaseUrl()];
	if (bucket.size() < MAX_IDLE_PER_HOST) {
		bucket.push_back(std::move(client));
	}
	// else: unique_ptr drops here, closing the connection in ~TailscaleHTTPClient.
}

void RegisterTailscaleHTTPUtil(DatabaseInstance &db) {
	// Make sure httpfs is the global util first, so non-tailnet traffic has a real
	// implementation (TLS / proxies / secrets) to delegate to. Best-effort: if httpfs is
	// unavailable we simply wrap whatever util is currently registered.
	ExtensionHelper::TryAutoLoadExtension(db, "httpfs");

	auto &current = db.config.GetHTTPUtil();
	if (current.GetName() == "Tailscale") {
		return; // already wrapped
	}
	auto util = make_shared_ptr<TailscaleHTTPUtil>(current);
	db.config.SetHTTPUtil(util);
}

} // namespace duckdb
