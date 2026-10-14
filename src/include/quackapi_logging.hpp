#pragma once

#include "duckdb/logging/logging.hpp"

namespace duckdb {

class DatabaseInstance;
struct QuackapiRequestRecord;

void EmitQuackapiAccessLog(DatabaseInstance &db, const QuackapiRequestRecord &entry, LogLevel level);

} // namespace duckdb
