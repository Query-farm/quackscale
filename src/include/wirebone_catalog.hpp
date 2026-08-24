#pragma once

#ifndef QUACKSCALE_WITH_WIREBONE
#define QUACKSCALE_WITH_WIREBONE 0
#endif

#include "duckdb.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/unique_ptr.hpp"
#include "duckdb/main/connection.hpp"
#include "duckdb/main/database.hpp"

#include <mutex>

namespace duckdb {

class ClientContext;

struct WireboneCatalogConfig {
	//! `duckdb` (default), `ducklake`, or `json`.
	string backend = "duckdb";
	//! Catalog that holds the `wirebone` schema. Empty = current database.
	string catalog;
	//! Optional dedicated DuckDB file (ignored for ducklake / current-db).
	string database;
};

//! Coordinator state as DuckDB tables (same process) or DuckLake (shared catalog).
class WireboneCatalog {
public:
	void Open(ClientContext &context, const WireboneCatalogConfig &config);
	void Close();
	bool Opened() const {
		return static_cast<bool>(con);
	}

	void EnsureSchema();
	string LoadSnapshot();
	void SaveSnapshot(const string &json);

	string SchemaName() const {
		return schema;
	}

private:
	void Run(const string &sql);
	string Qualify(const string &ident) const;
	string Escape(const string &value) const;

	unique_ptr<DuckDB> owned_db;
	unique_ptr<Connection> con;
	string schema = "wirebone";
	std::mutex write_mu;
};

} // namespace duckdb
