#include "tailscale_log_capture.hpp"

#include <unistd.h>

#include <cstring>

#ifdef QUACKSCALE_WITH_TAILSCALE
extern "C" {
#include "tailscale.h"
}
#endif

namespace duckdb {

namespace {

constexpr const char *LOGIN_URL_PREFIX = "https://login.tailscale.com/";

void AppendToBuffer(string &buffer, const char *data, size_t size) {
	buffer.append(data, size);
	auto pos = buffer.find(LOGIN_URL_PREFIX);
	if (pos == string::npos) {
		return;
	}
	auto end = buffer.find_first_of(" \t\r\n\"'", pos);
	if (end == string::npos) {
		end = buffer.size();
	}
	// Keep full buffer; ExtractLoginURL parses it.
}

} // namespace

void TailscaleLogCapture::Start(int tailscale_handle_p) {
#ifdef QUACKSCALE_WITH_TAILSCALE
	Stop();
	tailscale_handle = tailscale_handle_p;
	int fds[2];
	if (pipe(fds) != 0) {
		return;
	}
	read_fd = fds[0];
	write_fd = fds[1];
	stop_reader = false;
	if (tailscale_set_logfd(tailscale_handle, write_fd) != 0) {
		close(read_fd);
		close(write_fd);
		read_fd = -1;
		write_fd = -1;
		return;
	}
	reader_thread = std::thread([this]() { ReaderLoop(); });
#else
	(void)tailscale_handle_p;
#endif
}

void TailscaleLogCapture::Stop() {
	stop_reader = true;
#ifdef QUACKSCALE_WITH_TAILSCALE
	if (write_fd >= 0) {
		close(write_fd);
		write_fd = -1;
	}
#endif
	if (reader_thread.joinable()) {
		reader_thread.join();
	}
#ifdef QUACKSCALE_WITH_TAILSCALE
	if (read_fd >= 0) {
		close(read_fd);
		read_fd = -1;
	}
#endif
	tailscale_handle = -1;
}

void TailscaleLogCapture::ReaderLoop() {
	char chunk[512];
	while (!stop_reader) {
		if (read_fd < 0) {
			break;
		}
		auto n = read(read_fd, chunk, sizeof(chunk));
		if (n <= 0) {
			break;
		}
		std::lock_guard<std::mutex> guard(buffer_lock);
		AppendToBuffer(log_buffer, chunk, static_cast<size_t>(n));
	}
}

string TailscaleLogCapture::ExtractLoginURL() const {
	std::lock_guard<std::mutex> guard(buffer_lock);
	auto pos = log_buffer.find(LOGIN_URL_PREFIX);
	if (pos == string::npos) {
		return string();
	}
	auto end = log_buffer.find_first_of(" \t\r\n\"'", pos);
	if (end == string::npos) {
		return log_buffer.substr(pos);
	}
	return log_buffer.substr(pos, end - pos);
}

} // namespace duckdb
