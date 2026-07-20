#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

//! CREATE [OR REPLACE] ROUTE / DROP ROUTE syntax.
class RouteDdlParserExtension : public ParserExtension {
public:
	RouteDdlParserExtension();
};

//! The execution target the planner rewrites route DDL into. Registered so the
//! planned statement can resolve it by name.
TableFunction GetApplyRouteFunction();

//! CREATE [OR REPLACE] GROUP / DROP GROUP syntax (route groups / APIRouter).
class GroupDdlParserExtension : public ParserExtension {
public:
	GroupDdlParserExtension();
};

TableFunction GetApplyGroupFunction();
TableFunction GetQuackapiGroupsFunction();

} // namespace duckdb
