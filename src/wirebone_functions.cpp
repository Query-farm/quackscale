#include "wirebone_functions.hpp"

#include "tailscale_bridge.hpp"
#include "tailscale_http.hpp"
#include "wirebone_bridge.hpp"

#include "duckdb.hpp"
#include "duckdb/common/exception.hpp"
#include "duckdb/function/scalar_function.hpp"
#include "duckdb/function/table_function.hpp"
#include "duckdb/main/database.hpp"

namespace duckdb {

namespace {

static string NamedString(TableFunctionBindInput &input, const char *key, const string &fallback = string()) {
	auto it = input.named_parameters.find(key);
	if (it == input.named_parameters.end() || it->second.IsNull()) {
		return fallback;
	}
	return it->second.GetValue<string>();
}

static bool NamedBool(TableFunctionBindInput &input, const char *key, bool fallback) {
	auto it = input.named_parameters.find(key);
	if (it == input.named_parameters.end() || it->second.IsNull()) {
		return fallback;
	}
	return it->second.GetValue<bool>();
}

static void MaybeInstallHttpRoute(ClientContext &context, bool http_route) {
#ifdef QUACKSCALE_WITH_TAILSCALE
	if (http_route) {
		RegisterTailscaleHTTPUtil(DatabaseInstance::GetDatabase(context));
	}
#else
	(void)context;
	(void)http_route;
#endif
}

static WireboneServeConfig ParseServeConfig(TableFunctionBindInput &input) {
	WireboneServeConfig cfg;
	string listen = NamedString(input, "listen");
	if (!listen.empty()) {
		cfg.listen = listen;
	}
	string server_url = NamedString(input, "server_url");
	if (!server_url.empty()) {
		cfg.server_url = server_url;
	}
	cfg.state_path = NamedString(input, "state_path");
	if (cfg.state_path.empty()) {
		cfg.state_path = NamedString(input, "coordinator_state");
	}
	string domain = NamedString(input, "domain");
	if (!domain.empty()) {
		cfg.domain = domain;
	}
	if (input.named_parameters.find("dns_listen") != input.named_parameters.end()) {
		cfg.dns_listen = NamedString(input, "dns_listen");
	}
	string backend = NamedString(input, "backend");
	if (!backend.empty()) {
		cfg.backend = backend;
	}
	cfg.catalog = NamedString(input, "catalog");
	cfg.database = NamedString(input, "database");
	return cfg;
}

static TailscaleAuthConfig ParseJoinConfig(TableFunctionBindInput &input) {
	TailscaleAuthConfig config;
	if (!input.inputs.empty() && !input.inputs[0].IsNull()) {
		config.hostname = input.inputs[0].GetValue<string>();
	}
	string hostname = NamedString(input, "hostname");
	if (!hostname.empty()) {
		config.hostname = hostname;
	}
	config.authkey = NamedString(input, "authkey");
	config.control_url = NamedString(input, "control_url");
	config.state_dir = NamedString(input, "state_dir");
	config.ephemeral = NamedBool(input, "ephemeral", false);
	config.loopback_proxy = NamedBool(input, "loopback_proxy", false);
	return config;
}

static void RegisterServeParameters(TableFunction &function) {
	function.named_parameters["listen"] = LogicalType::VARCHAR;
	function.named_parameters["server_url"] = LogicalType::VARCHAR;
	function.named_parameters["state_path"] = LogicalType::VARCHAR;
	function.named_parameters["coordinator_state"] = LogicalType::VARCHAR;
	function.named_parameters["domain"] = LogicalType::VARCHAR;
	function.named_parameters["dns_listen"] = LogicalType::VARCHAR;
	function.named_parameters["backend"] = LogicalType::VARCHAR;
	function.named_parameters["catalog"] = LogicalType::VARCHAR;
	function.named_parameters["database"] = LogicalType::VARCHAR;
}

static void EmitServeRow(DataChunk &output, const WireboneServeStatus &st) {
	output.SetCardinality(1);
	output.SetValue(0, 0, st.bound.empty() ? Value() : Value(st.bound));
	output.SetValue(1, 0, st.control_url.empty() ? Value() : Value(st.control_url));
	output.SetValue(2, 0, st.domain.empty() ? Value() : Value(st.domain));
	output.SetValue(3, 0, st.preauth_key.empty() ? Value() : Value(st.preauth_key));
}

struct WireboneServeBindData : public TableFunctionData {
	WireboneServeConfig config;
	bool finished = false;
};

static unique_ptr<FunctionData> WireboneServeBind(ClientContext &, TableFunctionBindInput &input,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<WireboneServeBindData>();
	bind->config = ParseServeConfig(input);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"bound", "control_url", "domain", "preauth_key"};
	return std::move(bind);
}

static void WireboneServeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->CastNoConst<WireboneServeBindData>();
	if (bind.finished) {
		return;
	}
	auto st = WireboneBridge::Get().Serve(context, bind.config);
	EmitServeRow(output, st);
	bind.finished = true;
}

struct WireboneStatusBindData : public TableFunctionData {
	bool finished = false;
};

static unique_ptr<FunctionData> WireboneStatusBind(ClientContext &, TableFunctionBindInput &,
                                                   vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::BOOLEAN, LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::VARCHAR};
	names = {"linked", "running", "bound", "control_url", "domain", "preauth_key"};
	return make_uniq<WireboneStatusBindData>();
}

static void WireboneStatusFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->CastNoConst<WireboneStatusBindData>();
	if (bind.finished) {
		return;
	}
	auto st = WireboneBridge::Get().Status();
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(st.linked));
	output.SetValue(1, 0, Value::BOOLEAN(st.running));
	output.SetValue(2, 0, st.bound.empty() ? Value() : Value(st.bound));
	output.SetValue(3, 0, st.control_url.empty() ? Value() : Value(st.control_url));
	output.SetValue(4, 0, st.domain.empty() ? Value() : Value(st.domain));
	output.SetValue(5, 0, st.preauth_key.empty() ? Value() : Value(st.preauth_key));
	bind.finished = true;
}

struct WireboneStopBindData : public TableFunctionData {
	bool finished = false;
};

static unique_ptr<FunctionData> WireboneStopBind(ClientContext &, TableFunctionBindInput &,
                                                 vector<LogicalType> &return_types, vector<string> &names) {
	return_types = {LogicalType::BOOLEAN};
	names = {"stopped"};
	return make_uniq<WireboneStopBindData>();
}

static void WireboneStopFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->CastNoConst<WireboneStopBindData>();
	if (bind.finished) {
		return;
	}
	WireboneBridge::Get().Stop();
	output.SetCardinality(1);
	output.SetValue(0, 0, Value::BOOLEAN(true));
	bind.finished = true;
}

struct WirebonePreauthBindData : public TableFunctionData {
	bool reusable = true;
	bool ephemeral = false;
	bool finished = false;
};

static unique_ptr<FunctionData> WirebonePreauthBind(ClientContext &, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<WirebonePreauthBindData>();
	bind->reusable = NamedBool(input, "reusable", true);
	bind->ephemeral = NamedBool(input, "ephemeral", false);
	return_types = {LogicalType::VARCHAR, LogicalType::BOOLEAN, LogicalType::BOOLEAN};
	names = {"key", "reusable", "ephemeral"};
	return std::move(bind);
}

static void WirebonePreauthFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->CastNoConst<WirebonePreauthBindData>();
	if (bind.finished) {
		return;
	}
	auto key = WireboneBridge::Get().CreatePreauthKey(bind.reusable, bind.ephemeral);
	output.SetCardinality(1);
	output.SetValue(0, 0, Value(key));
	output.SetValue(1, 0, Value::BOOLEAN(bind.reusable));
	output.SetValue(2, 0, Value::BOOLEAN(bind.ephemeral));
	bind.finished = true;
}

struct WireboneNodesBindData : public TableFunctionData {
	vector<WireboneNodeRow> rows;
	idx_t offset = 0;
};

static unique_ptr<FunctionData> WireboneNodesBind(ClientContext &, TableFunctionBindInput &,
                                                  vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<WireboneNodesBindData>();
	if (WireboneBridge::Get().Linked() && WireboneBridge::Get().Status().running) {
		bind->rows = WireboneBridge::Get().Nodes();
	}
	return_types = {LogicalType::UBIGINT, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::VARCHAR, LogicalType::BOOLEAN};
	names = {"id", "hostname", "ipv4", "ipv6", "node_key", "online"};
	return std::move(bind);
}

static void WireboneNodesFunction(ClientContext &, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->CastNoConst<WireboneNodesBindData>();
	const idx_t count = MinValue<idx_t>(STANDARD_VECTOR_SIZE, bind.rows.size() - bind.offset);
	if (count == 0) {
		return;
	}
	for (idx_t i = 0; i < count; i++) {
		auto &row = bind.rows[bind.offset + i];
		output.SetValue(0, i, Value::UBIGINT(row.id));
		output.SetValue(1, i, row.hostname.empty() ? Value() : Value(row.hostname));
		output.SetValue(2, i, row.ipv4.empty() ? Value() : Value(row.ipv4));
		output.SetValue(3, i, row.ipv6.empty() ? Value() : Value(row.ipv6));
		output.SetValue(4, i, row.node_key.empty() ? Value() : Value(row.node_key));
		output.SetValue(5, i, Value::BOOLEAN(row.online));
	}
	output.SetCardinality(count);
	bind.offset += count;
}

static void WireboneBootstrapKeyFunction(DataChunk &, ExpressionState &, Vector &result) {
	if (!WireboneBridge::Get().Linked() || !WireboneBridge::Get().Status().running) {
		throw InvalidInputException("wirebone_bootstrap_key: CALL quackscale_hub() first");
	}
	result.Reference(Value(WireboneBridge::Get().BootstrapKey()));
}

static void RegisterTableAlias(ExtensionLoader &loader, TableFunction fn, const char *name) {
	fn.name = name;
	loader.RegisterFunction(fn);
}

struct QuackscaleServeBindData : public TableFunctionData {
	WireboneServeConfig serve;
	TailscaleAuthConfig join;
	bool do_join = true;
	bool http_route = true;
	bool finished = false;
};

static unique_ptr<FunctionData> QuackscaleServeBind(ClientContext &, TableFunctionBindInput &input,
                                                    vector<LogicalType> &return_types, vector<string> &names) {
	auto bind = make_uniq<QuackscaleServeBindData>();
	bind->serve = ParseServeConfig(input);
	bind->join = ParseJoinConfig(input);
	bind->do_join = NamedBool(input, "join", true);
	bind->http_route = NamedBool(input, "http_route", true);
	return_types = {LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR, LogicalType::VARCHAR,
	                LogicalType::BOOLEAN, LogicalType::VARCHAR, LogicalType::LIST(LogicalType::VARCHAR)};
	names = {"bound", "control_url", "domain", "preauth_key", "running", "hostname", "tailnet_ips"};
	return std::move(bind);
}

static void QuackscaleServeFunction(ClientContext &context, TableFunctionInput &data_p, DataChunk &output) {
	auto &bind = data_p.bind_data->CastNoConst<QuackscaleServeBindData>();
	if (bind.finished) {
		return;
	}

	auto st = WireboneBridge::Get().Serve(context, bind.serve);

	bool running = false;
	string hostname;
	vector<string> ips;
	if (bind.do_join) {
		if (bind.join.control_url.empty()) {
			bind.join.control_url = WireboneBridge::Get().LocalControlURL();
		}
		if (bind.join.authkey.empty()) {
			bind.join.authkey = st.preauth_key;
		}
		if (bind.join.hostname.empty()) {
			bind.join.hostname = "quackscale";
		}
		TailscaleBridge::Get().Up(bind.join);
		auto status = TailscaleBridge::Get().Status();
		running = status.running;
		hostname = status.hostname;
		ips = status.ips;
		if (running) {
			MaybeInstallHttpRoute(context, bind.http_route);
		}
	}

	vector<Value> ip_values;
	ip_values.reserve(ips.size());
	for (auto &ip : ips) {
		ip_values.emplace_back(ip);
	}

	output.SetCardinality(1);
	output.SetValue(0, 0, st.bound.empty() ? Value() : Value(st.bound));
	output.SetValue(1, 0, st.control_url.empty() ? Value() : Value(st.control_url));
	output.SetValue(2, 0, st.domain.empty() ? Value() : Value(st.domain));
	output.SetValue(3, 0, st.preauth_key.empty() ? Value() : Value(st.preauth_key));
	output.SetValue(4, 0, Value::BOOLEAN(running));
	output.SetValue(5, 0, hostname.empty() ? Value() : Value(hostname));
	output.SetValue(6, 0, Value::LIST(LogicalType::VARCHAR, std::move(ip_values)));
	bind.finished = true;
}

} // namespace

void RegisterWireboneFunctions(ExtensionLoader &loader) {
	TableFunction hub("quackscale_hub", {}, QuackscaleServeFunction, QuackscaleServeBind);
	RegisterServeParameters(hub);
	hub.named_parameters["hostname"] = LogicalType::VARCHAR;
	hub.named_parameters["authkey"] = LogicalType::VARCHAR;
	hub.named_parameters["control_url"] = LogicalType::VARCHAR;
	hub.named_parameters["state_dir"] = LogicalType::VARCHAR;
	hub.named_parameters["ephemeral"] = LogicalType::BOOLEAN;
	hub.named_parameters["loopback_proxy"] = LogicalType::BOOLEAN;
	hub.named_parameters["http_route"] = LogicalType::BOOLEAN;
	hub.named_parameters["join"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(hub);
	RegisterTableAlias(loader, hub, "quackscale_serve");

	TableFunction coord_only("wirebone_serve", {}, WireboneServeFunction, WireboneServeBind);
	RegisterServeParameters(coord_only);
	loader.RegisterFunction(coord_only);

	TableFunction status("quackscale_status", {}, WireboneStatusFunction, WireboneStatusBind);
	loader.RegisterFunction(status);
	RegisterTableAlias(loader, status, "wirebone_status");

	TableFunction stop("quackscale_stop", {}, WireboneStopFunction, WireboneStopBind);
	loader.RegisterFunction(stop);
	RegisterTableAlias(loader, stop, "wirebone_stop");

	TableFunction nodes("quackscale_nodes", {}, WireboneNodesFunction, WireboneNodesBind);
	loader.RegisterFunction(nodes);
	RegisterTableAlias(loader, nodes, "wirebone_nodes");

	TableFunction preauth("quackscale_preauth", {}, WirebonePreauthFunction, WirebonePreauthBind);
	preauth.named_parameters["reusable"] = LogicalType::BOOLEAN;
	preauth.named_parameters["ephemeral"] = LogicalType::BOOLEAN;
	loader.RegisterFunction(preauth);
	RegisterTableAlias(loader, preauth, "wirebone_preauth");

	loader.RegisterFunction(
	    ScalarFunction("wirebone_bootstrap_key", {}, LogicalType::VARCHAR, WireboneBootstrapKeyFunction));
}

} // namespace duckdb
