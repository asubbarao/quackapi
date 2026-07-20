#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

//! CREATE [OR REPLACE] QUEUE / DROP QUEUE syntax.
//! Grammar:
//!   CREATE [OR REPLACE] QUEUE <name>
//!     [WITH (max_attempts=<n>, visibility_timeout='30s'|30, backoff_base_seconds=<n>)]
//!   DROP QUEUE <name>
class QueueDdlParserExtension : public ParserExtension {
public:
	QueueDdlParserExtension();
};

//! Internal apply target for planned CREATE/DROP QUEUE.
TableFunction GetApplyQueueFunction();

//! Register quackapi_enqueue / dequeue / ack / nack / queues functions.
void RegisterQuackapiQueueFunctions(ExtensionLoader &loader);

//! Ensure the durable quackapi_jobs table + sequence exist (idempotent).
void EnsureQuackapiJobsTable(DatabaseInstance &db);

} // namespace duckdb
