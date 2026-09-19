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

//! Rewrite handler SQL so parsed references to policy-bound base tables become
//! secure subqueries (row filter + column masks). Uses policy expressions as-is
//! ($claims_* remain named parameters for the caller's verified-claims binder).
//!
//! When policies are configured, sources whose base-table identity cannot be
//! safely rewritten (for example views and table macros) fail closed. For an
//! unauthenticated access `deny_unauthenticated` is set; otherwise
//! `policy_error` describes the rejected source for server-side logging only.
string RewriteHandlerWithPolicies(DatabaseInstance &db, const string &handler_sql, bool authenticated,
                                  bool &deny_unauthenticated, string &policy_error);

//! Compatibility wrapper for callers that only need the unauthenticated result.
inline string RewriteHandlerWithPolicies(DatabaseInstance &db, const string &handler_sql, bool authenticated,
                                         bool &deny_unauthenticated) {
	string policy_error;
	return RewriteHandlerWithPolicies(db, handler_sql, authenticated, deny_unauthenticated, policy_error);
}

//! True when parsed handler SQL references a policy-bound table or an indirect
//! source that must be rejected while policies are active.
bool HandlerTouchesPoliciedTable(DatabaseInstance &db, const string &handler_sql);

//! True when policy enforcement cannot admit `handler_sql` at all: the rewriter
//! only expresses a single SELECT, and anything else reaching a bound relation
//! has no write policy to apply. `reason` names the relation. Checked at
//! CREATE ROUTE so the refusal lands where the handler is written; a handler
//! that reaches nothing bound is unaffected by policies existing elsewhere.
bool HandlerUnsupportedByPolicies(DatabaseInstance &db, const string &handler_sql, string &reason);

} // namespace duckdb
