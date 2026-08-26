#include "wirebone_bridge.hpp"

#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "tailscale_http.hpp"

#include <cstdlib>
#include <cstring>

#if QUACKSCALE_WITH_WIREBONE
extern "C" {
#include "wirebone/wirebone.h"
}
#endif

namespace duckdb {

namespace {

#if QUACKSCALE_WITH_WIREBONE
void PersistToCatalog(const char *json, void *user) {
	if (json && user) {
		static_cast<WireboneCatalog *>(user)->SaveSnapshot(json);
	}
}

string TakeCstr(char *p) {
	if (!p) {
		return {};
	}
	string s(p);
	std::free(p);
	return s;
}

string PortFromBound(const string &bound) {
	auto colon = bound.rfind(':');
	if (colon == string::npos || colon + 1 >= bound.size()) {
		return "8080";
	}
	return bound.substr(colon + 1);
}

string NormalizeMagicDnsDomain(const string &domain) {
	string d = StringUtil::Lower(domain);
	while (!d.empty() && d.back() == '.') {
		d.pop_back();
	}
	if (d.empty()) {
		return ".quackscale.local";
	}
	if (d.front() != '.') {
		d = "." + d;
	}
	return d;
}
#endif

} // namespace

WireboneBridge::~WireboneBridge() {
	Stop();
}

WireboneBridge &WireboneBridge::Get() {
	static WireboneBridge instance;
	return instance;
}

bool WireboneBridge::Linked() const {
#if QUACKSCALE_WITH_WIREBONE
	return true;
#else
	return false;
#endif
}

void WireboneBridge::RequireLinked() const {
#if !QUACKSCALE_WITH_WIREBONE
	throw NotImplementedException(
	    "QuackScale was built without Wirebone (QUACKSCALE_WITH_WIREBONE=OFF). "
	    "Checkout wirebone.cpp next to this repo (or set QUACKSCALE_WIREBONE_DIR) and rebuild "
	    "with OpenSSL, nghttp2, and libzstd.");
#endif
}

WireboneServeStatus WireboneBridge::Status() const {
	std::lock_guard<std::mutex> g(mu);
	WireboneServeStatus st = last;
	st.linked = Linked();
#if QUACKSCALE_WITH_WIREBONE
	st.running = coordinator && wirebone_running(static_cast<wirebone_coordinator *>(coordinator));
#else
	st.running = false;
#endif
	return st;
}

WireboneServeStatus WireboneBridge::Serve(ClientContext &context, const WireboneServeConfig &config) {
	RequireLinked();
#if QUACKSCALE_WITH_WIREBONE
	std::lock_guard<std::mutex> g(mu);
	if (coordinator) {
		throw InvalidInputException("quackscale hub already running; CALL quackscale_stop() first");
	}

	string backend = StringUtil::Lower(config.backend);
	if (backend.empty()) {
		backend = "duckdb";
	}
	if (backend != "duckdb" && backend != "ducklake" && backend != "json") {
		throw InvalidInputException("quackscale hub backend must be duckdb, ducklake, or json");
	}
	if (backend == "ducklake" && config.catalog.empty()) {
		throw InvalidInputException("quackscale hub backend=ducklake requires catalog => '<attached ducklake>'");
	}

	const bool use_catalog = backend != "json";
	if (use_catalog) {
		WireboneCatalogConfig cat_cfg;
		cat_cfg.backend = backend;
		cat_cfg.catalog = config.catalog;
		cat_cfg.database = config.database;
		catalog.Open(context, cat_cfg);
		catalog.EnsureSchema();
	}

	wirebone_config cfg {};
	cfg.listen = config.listen.c_str();
	cfg.server_url = config.server_url.c_str();
	cfg.state_path = use_catalog ? "" : config.state_path.c_str();
	cfg.domain = config.domain.c_str();
	cfg.dns_listen = config.dns_listen.c_str();

	auto *c = wirebone_create(&cfg);
	if (!c) {
		catalog.Close();
		throw IOException("quackscale_hub failed to create the control plane");
	}
	if (use_catalog) {
		auto snap = catalog.LoadSnapshot();
		if (!snap.empty() && wirebone_import_state(c, snap.c_str()) != 0) {
			wirebone_destroy(c);
			catalog.Close();
			throw IOException("quackscale_hub failed to import DuckDB snapshot");
		}
		wirebone_set_persist_callback(c, PersistToCatalog, &catalog);
		wirebone_persist(c);
	}
	if (wirebone_start(c) != 0) {
		wirebone_destroy(c);
		catalog.Close();
		throw IOException("quackscale_hub failed to start on %s", config.listen);
	}

	coordinator = c;
	last.linked = true;
	last.running = true;
	last.bound = TakeCstr(wirebone_bound_address(c));
	last.control_url = config.server_url;
	last.domain = config.domain;
	last.preauth_key = TakeCstr(wirebone_bootstrap_key(c));
	if (last.preauth_key.empty()) {
		last.preauth_key = TakeCstr(wirebone_create_preauth_key(c, 1, 0));
	}

	RegisterTailnetMagicDnsSuffix(NormalizeMagicDnsDomain(config.domain));
	return last;
#else
	(void)context;
	(void)config;
	return Status();
#endif
}

void WireboneBridge::Stop() {
#if QUACKSCALE_WITH_WIREBONE
	std::lock_guard<std::mutex> g(mu);
	if (!coordinator) {
		last.running = false;
		return;
	}
	auto *c = static_cast<wirebone_coordinator *>(coordinator);
	wirebone_persist(c);
	wirebone_stop(c);
	wirebone_destroy(c);
	coordinator = nullptr;
	catalog.Close();
	last.running = false;
#endif
}

string WireboneBridge::BootstrapKey() const {
	RequireLinked();
#if QUACKSCALE_WITH_WIREBONE
	std::lock_guard<std::mutex> g(mu);
	if (!coordinator) {
		throw InvalidInputException("quackscale hub is not running; CALL quackscale_hub() first");
	}
	string key = TakeCstr(wirebone_bootstrap_key(static_cast<wirebone_coordinator *>(coordinator)));
	return key.empty() ? last.preauth_key : key;
#else
	return {};
#endif
}

string WireboneBridge::CreatePreauthKey(bool reusable, bool ephemeral, const string &token, int32_t shared) {
	RequireLinked();
#if QUACKSCALE_WITH_WIREBONE
	std::lock_guard<std::mutex> g(mu);
	if (!coordinator) {
		throw InvalidInputException("quackscale hub is not running; CALL quackscale_hub() first");
	}
	string key =
	    TakeCstr(wirebone_create_preauth_key_ex(static_cast<wirebone_coordinator *>(coordinator), reusable ? 1 : 0,
	                                            ephemeral ? 1 : 0, token.empty() ? nullptr : token.c_str(), shared));
	if (key.empty()) {
		throw IOException("quackscale_preauth failed to create a key");
	}
	if (last.preauth_key.empty()) {
		last.preauth_key = key;
	}
	return key;
#else
	(void)reusable;
	(void)ephemeral;
	(void)token;
	(void)shared;
	return {};
#endif
}

vector<WireboneNodeRow> WireboneBridge::Nodes() const {
	RequireLinked();
#if QUACKSCALE_WITH_WIREBONE
	std::lock_guard<std::mutex> g(mu);
	vector<WireboneNodeRow> out;
	if (!coordinator) {
		return out;
	}
	size_t count = 0;
	wirebone_node_info *nodes = wirebone_list_nodes(static_cast<const wirebone_coordinator *>(coordinator), &count);
	out.reserve(count);
	for (size_t i = 0; i < count; ++i) {
		WireboneNodeRow row;
		row.id = nodes[i].id;
		if (nodes[i].hostname) {
			row.hostname = nodes[i].hostname;
		}
		if (nodes[i].ipv4) {
			row.ipv4 = nodes[i].ipv4;
		}
		if (nodes[i].ipv6) {
			row.ipv6 = nodes[i].ipv6;
		}
		if (nodes[i].node_key) {
			row.node_key = nodes[i].node_key;
		}
		row.online = nodes[i].online != 0;
		if (nodes[i].token) {
			row.token = nodes[i].token;
		}
		row.shared = nodes[i].shared != 0;
		out.push_back(std::move(row));
	}
	wirebone_free_nodes(nodes, count);
	return out;
#else
	return {};
#endif
}

string WireboneBridge::LocalControlURL() const {
	std::lock_guard<std::mutex> g(mu);
	if (last.bound.empty()) {
		return last.control_url;
	}
#if QUACKSCALE_WITH_WIREBONE
	return "http://127.0.0.1:" + PortFromBound(last.bound);
#else
	return last.control_url;
#endif
}

} // namespace duckdb
