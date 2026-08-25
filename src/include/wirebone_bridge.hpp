#pragma once

#ifndef QUACKSCALE_WITH_WIREBONE
#define QUACKSCALE_WITH_WIREBONE 0
#endif

#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "wirebone_catalog.hpp"

#include <cstdint>
#include <mutex>

namespace duckdb {

class ClientContext;

struct WireboneServeConfig {
	string listen = "0.0.0.0:8080";
	string server_url = "http://127.0.0.1:8080";
	string state_path;
	string domain = "quackscale.local";
	string dns_listen = "0.0.0.0:5353";
	//! `duckdb` (default), `ducklake`, or `json`.
	string backend = "duckdb";
	string catalog;
	string database;
};

struct WireboneServeStatus {
	bool linked = false;
	bool running = false;
	string bound;
	string control_url;
	string domain;
	string preauth_key;
};

struct WireboneNodeRow {
	uint64_t id = 0;
	string hostname;
	string ipv4;
	string ipv6;
	string node_key;
	bool online = false;
	string token;
	bool shared = false;
};

//! In-process Tailscale/Headscale-compatible control plane (Wirebone).
//! One DuckDB process can host this and still join as a tsnet client via TailscaleBridge.
class WireboneBridge {
public:
	static WireboneBridge &Get();

	bool Linked() const;
	WireboneServeStatus Status() const;
	WireboneServeStatus Serve(ClientContext &context, const WireboneServeConfig &config);
	void Stop();
	string BootstrapKey() const;
	string CreatePreauthKey(bool reusable, bool ephemeral, const string &token = string(), int shared = -1);
	vector<WireboneNodeRow> Nodes() const;
	//! Loopback control URL for the in-process client (http://127.0.0.1:<port>).
	string LocalControlURL() const;

private:
	WireboneBridge() = default;
	~WireboneBridge();

	WireboneBridge(const WireboneBridge &) = delete;
	WireboneBridge &operator=(const WireboneBridge &) = delete;

	void RequireLinked() const;

	mutable std::mutex mu;
#if QUACKSCALE_WITH_WIREBONE
	void *coordinator = nullptr; // wirebone_coordinator*
	WireboneCatalog catalog;
#endif
	WireboneServeStatus last;
};

} // namespace duckdb
