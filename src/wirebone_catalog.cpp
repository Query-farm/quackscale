#include "wirebone_catalog.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/common/string_util.hpp"
#include "duckdb/main/client_context.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/materialized_query_result.hpp"

#if QUACKSCALE_WITH_WIREBONE
#include <nlohmann/json.hpp>
#include <cstdint>
#include <string>
#endif

namespace duckdb {

void WireboneCatalog::Open(ClientContext &context, const WireboneCatalogConfig &config) {
	Close();
	if (!config.catalog.empty() && config.catalog != "memory" && config.catalog != "current") {
		schema = config.catalog + ".quackscale";
	} else {
		schema = "quackscale";
	}
	if (!config.database.empty() && config.backend != "ducklake") {
		owned_db = make_uniq<DuckDB>(config.database);
		con = make_uniq<Connection>(*owned_db);
	} else {
		con = make_uniq<Connection>(DatabaseInstance::GetDatabase(context));
	}
}

void WireboneCatalog::Close() {
	std::lock_guard<std::mutex> g(write_mu);
	con.reset();
	owned_db.reset();
	schema = "quackscale";
}

void WireboneCatalog::Run(const string &sql) {
	auto result = con->Query(sql);
	if (result->HasError()) {
		throw IOException("quackscale hub catalog: %s\n%s", result->GetError(), sql);
	}
}

string WireboneCatalog::Qualify(const string &ident) const {
	return schema + "." + ident;
}

string WireboneCatalog::Escape(const string &value) const {
	return StringUtil::Replace(value, "'", "''");
}

void WireboneCatalog::EnsureSchema() {
	if (!con) {
		return;
	}
	std::lock_guard<std::mutex> g(write_mu);
	Run("CREATE SCHEMA IF NOT EXISTS " + schema);
	Run("CREATE TABLE IF NOT EXISTS " + Qualify("meta") + " (k VARCHAR PRIMARY KEY, v VARCHAR)");
	Run("CREATE TABLE IF NOT EXISTS " + Qualify("preauth_keys") +
	    " (key VARCHAR PRIMARY KEY, reusable BOOLEAN, ephemeral BOOLEAN, used BOOLEAN, expires_unix BIGINT, "
	    "token VARCHAR, shared BOOLEAN)");
	Run("CREATE TABLE IF NOT EXISTS " + Qualify("nodes") +
	    " (id UBIGINT PRIMARY KEY, stable_id VARCHAR, hostname VARCHAR, machine_key VARCHAR, node_key VARCHAR, "
	    "disco_key VARCHAR, ipv4 VARCHAR, ipv6 VARCHAR, endpoints VARCHAR, online BOOLEAN, ephemeral BOOLEAN, "
	    "token VARCHAR, shared BOOLEAN)");
	Run("ALTER TABLE " + Qualify("preauth_keys") + " ADD COLUMN IF NOT EXISTS token VARCHAR DEFAULT ''");
	Run("ALTER TABLE " + Qualify("preauth_keys") + " ADD COLUMN IF NOT EXISTS shared BOOLEAN DEFAULT false");
	Run("ALTER TABLE " + Qualify("nodes") + " ADD COLUMN IF NOT EXISTS token VARCHAR DEFAULT ''");
	Run("ALTER TABLE " + Qualify("nodes") + " ADD COLUMN IF NOT EXISTS shared BOOLEAN DEFAULT false");

	// One-cycle alias so older SQL that reads wirebone.* still works.
	string alias_schema = "wirebone";
	if (schema.size() > 10 && schema.rfind(".quackscale") == schema.size() - 11) {
		alias_schema = schema.substr(0, schema.size() - 11) + ".wirebone";
	}
	if (alias_schema != schema) {
		Run("CREATE SCHEMA IF NOT EXISTS " + alias_schema);
		Run("CREATE OR REPLACE VIEW " + alias_schema + ".meta AS SELECT * FROM " + Qualify("meta"));
		Run("CREATE OR REPLACE VIEW " + alias_schema + ".preauth_keys AS SELECT * FROM " + Qualify("preauth_keys"));
		Run("CREATE OR REPLACE VIEW " + alias_schema + ".nodes AS SELECT * FROM " + Qualify("nodes"));
	}
}

string WireboneCatalog::LoadSnapshot() {
	if (!con) {
		return {};
	}
	std::lock_guard<std::mutex> g(write_mu);
	auto result = con->Query("SELECT v FROM " + Qualify("meta") + " WHERE k = 'snapshot'");
	if (result->HasError() || result->RowCount() == 0) {
		return {};
	}
	return result->GetValue(0, 0).ToString();
}

void WireboneCatalog::SaveSnapshot(const string &json) {
	if (!con) {
		return;
	}
#if QUACKSCALE_WITH_WIREBONE
	std::lock_guard<std::mutex> g(write_mu);
	Run("BEGIN TRANSACTION");
	try {
		Run("INSERT OR REPLACE INTO " + Qualify("meta") + " VALUES ('snapshot', '" + Escape(json) + "')");
		nlohmann::json j = nlohmann::json::parse(json.empty() ? "{}" : json);
		if (j.contains("noise_private")) {
			Run("INSERT OR REPLACE INTO " + Qualify("meta") + " VALUES ('noise_private', '" +
			    Escape(j["noise_private"].get<std::string>()) + "')");
		}
		if (j.contains("next_node_id")) {
			Run("INSERT OR REPLACE INTO " + Qualify("meta") + " VALUES ('next_node_id', '" +
			    std::to_string(j["next_node_id"].get<uint64_t>()) + "')");
		}
		if (j.contains("next_ip_index")) {
			Run("INSERT OR REPLACE INTO " + Qualify("meta") + " VALUES ('next_ip_index', '" +
			    std::to_string(j["next_ip_index"].get<uint32_t>()) + "')");
		}
		Run("DELETE FROM " + Qualify("preauth_keys"));
		if (j.contains("preauth_keys")) {
			for (const auto &k : j["preauth_keys"]) {
				Run("INSERT INTO " + Qualify("preauth_keys") + " VALUES ('" + Escape(k.value("key", std::string())) +
				    "', " + string(k.value("reusable", true) ? "true" : "false") + ", " +
				    string(k.value("ephemeral", false) ? "true" : "false") + ", " +
				    string(k.value("used", false) ? "true" : "false") + ", " +
				    std::to_string(k.value("expires_unix", 0)) + ", '" + Escape(k.value("token", std::string())) +
				    "', " + string(k.value("shared", k.value("token", std::string()).empty()) ? "true" : "false") +
				    ")");
			}
		}
		Run("DELETE FROM " + Qualify("nodes"));
		if (j.contains("nodes")) {
			for (const auto &n : j["nodes"]) {
				std::string endpoints;
				if (n.contains("endpoints")) {
					endpoints = n["endpoints"].dump();
				}
				Run("INSERT INTO " + Qualify("nodes") + " VALUES (" + std::to_string(n.value("id", 0)) + ", '" +
				    Escape(n.value("stable_id", std::string())) + "', '" + Escape(n.value("hostname", std::string())) +
				    "', '" + Escape(n.value("machine_key", std::string())) + "', '" +
				    Escape(n.value("node_key", std::string())) + "', '" + Escape(n.value("disco_key", std::string())) +
				    "', '" + Escape(n.value("ipv4", std::string())) + "', '" + Escape(n.value("ipv6", std::string())) +
				    "', '" + Escape(endpoints) + "', " + string(n.value("online", false) ? "true" : "false") + ", " +
				    string(n.value("ephemeral", false) ? "true" : "false") + ", '" +
				    Escape(n.value("token", std::string())) + "', " +
				    string(n.value("shared", n.value("token", std::string()).empty()) ? "true" : "false") + ")");
			}
		}
		Run("COMMIT");
	} catch (...) {
		try {
			con->Query("ROLLBACK");
		} catch (...) {
		}
		throw;
	}
#else
	(void)json;
#endif
}

} // namespace duckdb
