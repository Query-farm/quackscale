#pragma once

#include "duckdb/common/http_util.hpp"
#include "duckdb/common/unordered_map.hpp"
#include "duckdb/common/vector.hpp"

#include <mutex>

namespace duckdb {

class DatabaseInstance;

//! True if `proto_host_port` (e.g. "http://100.95.32.19:9494") names a tailnet host:
//! an IPv4 in the CGNAT range 100.64.0.0/10, or a *.ts.net MagicDNS name. Scheme and
//! :port are stripped before the test. NOTE: bare MagicDNS short names ("lake-server")
//! are NOT matched — those still need tailscale_quack_forward.
bool IsTailnetHost(const string &proto_host_port);

//! Install TailscaleHTTPUtil as the database's global HTTP util, wrapping whatever util
//! is currently registered (httpfs, after auto-load). Idempotent: a second call is a
//! no-op. Called from tailscale_up once the node is up unless http_route => false.
void RegisterTailscaleHTTPUtil(DatabaseInstance &db);

//! HTTP/1.1 client that speaks plaintext over a tailscale_dial'd fd. The tailnet is the
//! encryption layer, so we never negotiate TLS here regardless of URL scheme. Holds one
//! keep-alive connection open across requests and frames responses by Content-Length or
//! chunked transfer-encoding; a stale pooled connection is transparently redialed once.
class TailscaleHTTPClient : public HTTPClient {
public:
	explicit TailscaleHTTPClient(const string &proto_host_port);
	~TailscaleHTTPClient() override;

	void Initialize(HTTPParams &http_params) override;

	unique_ptr<HTTPResponse> Get(GetRequestInfo &info) override;
	unique_ptr<HTTPResponse> Post(PostRequestInfo &info) override;
	unique_ptr<HTTPResponse> Put(PutRequestInfo &info) override;
	unique_ptr<HTTPResponse> Head(HeadRequestInfo &info) override;
	unique_ptr<HTTPResponse> Delete(DeleteRequestInfo &info) override;

	//! Parsed HTTP response. Public so the file-local ToHTTPResponse() helper can move from it.
	struct ParsedResponse {
		int status = 0;
		string reason;
		HTTPHeaders headers;
		string body;
		//! The peer signalled (or the framing implies) that this connection must not be reused.
		bool connection_close = false;
	};

private:
	//! Send one request and read the full response, redialing once if a *reused* keep-alive
	//! connection turns out to be dead. `has_body` requests carry Content-Length framing.
	ParsedResponse RoundTrip(const string &method, const string &path, const HTTPHeaders &headers,
	                         const_data_ptr_t body, idx_t body_len, bool has_body);

	void CloseConn();
	bool ReadMore();                            //!< append one chunk of socket data into rx; false on EOF/error
	bool ReadResponse(const string &method, ParsedResponse &out);
	bool ReadChunkedBody(string &body);

	string host; //!< parsed from base_url, scheme + port stripped
	string port; //!< defaults to "80" if the URL omits it
	int fd = -1; //!< persistent keep-alive connection, -1 == not connected
	string rx;   //!< bytes read from fd but not yet consumed (carries across requests)
	idx_t timeout_seconds = 30;
};

//! Global HTTP util that intercepts tailnet hosts and delegates everything else to the
//! previously-registered util (httpfs), preserving real TLS / proxies / secrets and its
//! keep-alive cache for non-tailnet traffic. Maintains a small idle pool of tailnet
//! clients so keep-alive survives across DuckDB file handles.
class TailscaleHTTPUtil : public HTTPUtil {
public:
	explicit TailscaleHTTPUtil(HTTPUtil &prev) : prev(prev) {
	}

	string GetName() const override {
		return "Tailscale";
	}

	unique_ptr<HTTPParams> InitializeParameters(DatabaseInstance &db, const string &path) override {
		return prev.InitializeParameters(db, path);
	}
	unique_ptr<HTTPParams> InitializeParameters(ClientContext &context, const string &path) override {
		return prev.InitializeParameters(context, path);
	}
	unique_ptr<HTTPParams> InitializeParameters(optional_ptr<FileOpener> opener,
	                                            optional_ptr<FileOpenerInfo> info) override {
		return prev.InitializeParameters(opener, info);
	}

	unique_ptr<HTTPClient> InitializeClient(HTTPParams &http_params, const string &proto_host_port) override;
	void CloseClient(unique_ptr<HTTPClient> &&client) override;

private:
	//! Reference to the previous global util. After SetHTTPUtil() the old util is retained
	//! in DBConfig (old_http_utils) for the DB lifetime, so this ref stays valid.
	HTTPUtil &prev;

	//! Idle keep-alive clients keyed by proto_host_port, capped at MAX_IDLE_PER_HOST each.
	static constexpr idx_t MAX_IDLE_PER_HOST = 8;
	std::mutex pool_mutex;
	unordered_map<string, vector<unique_ptr<HTTPClient>>> idle_clients;
};

} // namespace duckdb
