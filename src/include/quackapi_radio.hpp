#pragma once

#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

//! The topic broker the `radio` community extension talks to.
//!
//! radio is a WebSocket *client* with no server of its own (`grep -rn "Server"
//! radio/src` is empty): it dials a URL, buffers what arrives, and hands the
//! buffer back as rows. quackapi is the half radio does not have. These four
//! functions are the fan-out behind a `CREATE STREAM … WS` endpoint, so a
//! remote DuckDB with only `radio` loaded can subscribe to a topic here and
//! query the messages this server publishes.
//!
//! State is process-global and outlives any one DuckDB connection, because the
//! publisher and the socket sessions are different connections in the same
//! process. That matches radio's own model on the other end of the wire.
void RegisterQuackapiRadioFunctions(ExtensionLoader &loader);

} // namespace duckdb
