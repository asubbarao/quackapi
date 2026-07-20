#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

//! CREATE [OR REPLACE] STREAM / DROP STREAM syntax.
//! Grammar:
//!   CREATE [OR REPLACE] STREAM <name> GET '<path>'
//!     [WITH (interval='1s'|1000)]
//!     AS <select>
//!   DROP STREAM <name>
//!
//! SSE only (text/event-stream). WebSocket is not supported on bundled
//! cpp-httplib — CREATE STREAM … WS is rejected with a clear error.
class StreamDdlParserExtension : public ParserExtension {
public:
	StreamDdlParserExtension();
};

TableFunction GetApplyStreamFunction();

//! Register quackapi_streams() inspection table function.
void RegisterQuackapiStreamFunctions(ExtensionLoader &loader);

} // namespace duckdb
