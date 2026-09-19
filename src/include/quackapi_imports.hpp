//===----------------------------------------------------------------------===//
// quackapi_imports.hpp
//
// The companions quackapi composes instead of reimplementing, and the single
// gate that decides a missing one is a startup error.
//
// Nothing here ever INSTALLs. A download inside serve or a handler turns an
// offline box (or a renamed community package) into a 404 the server then
// shrugs off. LOAD is local and idempotent; the catalog probe afterwards is the
// active thing — the symbol quackapi is about to call.
//===----------------------------------------------------------------------===//
#pragma once

#include "duckdb/common/optional_ptr.hpp"
#include "duckdb/common/string.hpp"
#include "duckdb/main/database.hpp"
#include "duckdb/main/extension/extension_loader.hpp"

namespace duckdb {

class ClientContext;

//! LOAD a companion (never INSTALL), then prove the function quackapi calls is
//! in the catalog. Throws naming the extension and the feature when either step
//! fails, so an operator sees what to install instead of a missing symbol.
void QuackapiRequireExtension(DatabaseInstance &db, const string &extension, const string &probe_function,
                              const string &feature);

//! Same gate for a companion that registers settings rather than functions.
void QuackapiRequireExtensionSetting(DatabaseInstance &db, const string &extension, const string &probe_setting,
                                     const string &feature);

//! Reconcile the OTLP endpoint with quackapi_otlp / quackapi_otlp_catalog.
//! context: the session whose SET is being honoured — a settings read off
//! DBConfig alone sees the database default, never the caller's SET. Null at
//! LOAD, where no session exists and the defaults are all there is.
//! enforce: an explicitly configured endpoint is mandatory and a missing otlp
//! throws. LOAD quackapi may not fail over a missing companion, so the local
//! default reports on stderr instead.
void QuackapiOtlpReconcile(DatabaseInstance &db, optional_ptr<ClientContext> context, bool enforce);

//! quackapi_otlp() and quackapi_queue_worker().
void RegisterQuackapiImportFunctions(ExtensionLoader &loader);

} // namespace duckdb
