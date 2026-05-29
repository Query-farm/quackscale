#pragma once

#include "duckdb/common/string.hpp"

#include <atomic>
#include <mutex>
#include <thread>

namespace duckdb {

//! Captures tsnet log lines via tailscale_set_logfd to extract interactive login URLs.
class TailscaleLogCapture {
public:
	void Start(int tailscale_handle);
	void Stop();
	string ExtractLoginURL() const;

private:
	void ReaderLoop();

	int read_fd = -1;
	int write_fd = -1;
	int tailscale_handle = -1;
	std::thread reader_thread;
	std::atomic<bool> stop_reader {false};
	mutable std::mutex buffer_lock;
	string log_buffer;
};

} // namespace duckdb
