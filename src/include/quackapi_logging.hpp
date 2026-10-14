#pragma once

namespace duckdb {

struct QuackapiRequestRecord;

void EmitAccessLogStderr(const QuackapiRequestRecord &entry);

} // namespace duckdb
