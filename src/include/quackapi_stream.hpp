#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/main/extension/extension_loader.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

//! CREATE [OR REPLACE] STREAM / DROP STREAM syntax.
//! Grammar:
//!   CREATE [OR REPLACE] STREAM <name> (GET|WS) '<path>'
//!     [WITH (interval='1s'|1000)]
//!     AS <select>
//!   DROP STREAM <name>
//!
//! GET is Server-Sent Events (text/event-stream). WS is an RFC 6455 WebSocket,
//! answered from QuackapiHttplibServer::process_and_close_socket. A WS handler
//! that binds $message answers inbound frames instead of pushing; one that does
//! not pushes rows exactly as the SSE transport does.
class StreamDdlParserExtension : public ParserExtension {
public:
	StreamDdlParserExtension();
};

TableFunction GetApplyStreamFunction();

//! Register quackapi_streams() inspection table function.
void RegisterQuackapiStreamFunctions(ExtensionLoader &loader);

} // namespace duckdb
