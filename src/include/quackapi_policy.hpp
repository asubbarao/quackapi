#pragma once

#include "duckdb/function/table_function.hpp"
#include "duckdb/parser/parser_extension.hpp"

namespace duckdb {

class DatabaseInstance;
class ExtensionLoader;

//! CREATE / DROP ROW ACCESS POLICY, CREATE / DROP MASKING POLICY,
//! ALTER TABLE … ADD/DROP ROW ACCESS POLICY, ALTER TABLE … SET/UNSET MASKING POLICY.
class PolicyDdlParserExtension : public ParserExtension {
public:
	PolicyDdlParserExtension();
};

TableFunction GetApplyPolicyFunction();
TableFunction GetQuackapiPoliciesFunction();

//! Rewrite handler SQL so references to policied tables become secure subqueries
//! (row filter + column masks). Uses policy expressions as-is ($claims_* stay
//! named parameters for the existing claims binder).
//!
//! If the handler touches a policied table and `authenticated` is false, sets
//! `deny_unauthenticated` so the server can return 403 (fail closed).
string RewriteHandlerWithPolicies(DatabaseInstance &db, const string &handler_sql, bool authenticated,
                                  bool &deny_unauthenticated);

//! True when `handler_sql` references any table that has a row-access or masking binding.
bool HandlerTouchesPoliciedTable(DatabaseInstance &db, const string &handler_sql);

} // namespace duckdb
