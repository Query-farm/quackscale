#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

void RegisterWireboneFunctions(ExtensionLoader &loader);

} // namespace duckdb
