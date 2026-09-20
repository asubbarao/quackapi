//===----------------------------------------------------------------------===//
// quackapi_events.hpp
//
// Per-request observability outside the process, through the `events`
// companion. quackapi answers every HTTP request on a connection of its own
// and destroys it at the end, so that connection's lifecycle, its query and
// its transaction ARE the request — and `events` reports all three to a
// handler program quackapi never links.
//
// What arrives is query and transaction lifecycle only. There is no row,
// table or CDC event here, and no plan to fake one.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/common/vector.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class ClientContext;
class Connection;

//! The sink as quackapi_events() reports it, and as reconcile left it.
struct QuackapiEventsSink {
	//! Command line `events` spawns per event, writing one JSON object to its
	//! stdin. Empty when the sink is off.
	string destination;
	//! Event types asked of events_types.
	vector<string> types;
	//! Fire-and-forget. True buys latency and gives up delivery entirely.
	bool async = false;
	//! off | serving | unavailable
	string state = "off";
	string detail;
};

//! Move events_destination / events_types / events_async to what
//! quackapi_events, quackapi_events_types and quackapi_events_async ask for.
//!
//! The move is GLOBAL on purpose: a plain SET is session-local, and a request
//! runs on a connection the caller never touched, so a session-local
//! destination reaches nothing quackapi serves.
//!
//! context: the session whose SET is being honoured; null where none exists.
//! enforce: a configured sink is a promise quackapi_serve keeps or refuses —
//! a missing `events` extension throws there instead of serving blind.
void QuackapiEventsReconcile(DatabaseInstance &db, optional_ptr<ClientContext> context, bool enforce,
                             QuackapiEventsSink &sink);

//! Name this request's connection with the id the client sees in
//! X-Request-ID, so every event it emits carries it — including the events of
//! a request that fails, whose body says nothing. No-op when the sink is off.
void QuackapiEventsStampRequest(Connection &con, const string &request_id);

//! quackapi_events().
void RegisterQuackapiEventsFunctions(ExtensionLoader &loader);

} // namespace duckdb
