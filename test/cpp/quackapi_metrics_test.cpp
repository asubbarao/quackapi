#include "duckdb.hpp"
#include "duckdb/main/client_context.hpp"
#include "quackapi_query_log.hpp"
#include <iostream>
#include <stdexcept>
using namespace duckdb;
static unique_ptr<QueryResult> Run(Connection &con, const string &sql) {
	auto r = con.Query(sql);
	if (r->HasError())
		throw std::runtime_error(r->GetError());
	return r;
}
int main(int argc, char **argv) {
	try {
		if (argc < 2)
			throw std::runtime_error("expected explicit candidate path");
		DBConfig config;
		config.SetOptionByName("allow_unsigned_extensions", Value::BOOLEAN(true));
		DuckDB db(nullptr, &config);
		Connection control(db);
		Run(control, string("LOAD '") + argv[1] + "'");
		Connection before_logging(db);
		Run(control, "CALL enable_logging(['QueryLog','Metrics'],storage:='memory',storage_buffer_size:=0)");
		Run(before_logging, "SELECT 43125 AS existing_connection_probe");
		{
			Connection work(db);
			Run(work, "SELECT 43123 AS metrics_callback_probe");
			Run(work, "SELECT quackapi_selfdispatch('SELECT 43124 AS nested_metrics_probe') AS receipt");
			auto failed = work.Query("SELECT missing_column AS failed_metrics_probe");
			if (!failed->HasError())
				throw std::runtime_error("expected binder error");
		}
		Run(control,
		    "SELECT CASE WHEN count(message)<>5 THEN error('five exact SQL events required') END FROM duckdb_logs "
		    "WHERE type='Metrics' AND parse_duckdb_log_message('Metrics',message).metric='statement.sql'");
		Run(control,
		    "SELECT CASE WHEN count(message)<>1 THEN error('binder failure error missing') END FROM duckdb_logs WHERE "
		    "type='Metrics' AND parse_duckdb_log_message('Metrics',message).metric='statement.error' AND "
		    "contains(parse_duckdb_log_message('Metrics',message).value,'missing_column')");
		Run(control, "SELECT CASE WHEN count_if(query_id IS NULL)>0 THEN error('query identity missing') END FROM "
		             "duckdb_logs WHERE type='Metrics'");
		Run(control,
		    R"SQL(SELECT quackapi_selfdispatch($body$SELECT quackapi_selfdispatch('SELECT 43126 AS scalar_origin_probe').status AS child_status$body$))SQL");
		Run(control,
		    R"SQL(CREATE ROUTE metrics_audit POST '/metrics-audit' PARAM sql VARCHAR MIN_LENGTH 1 AS $sql)SQL");
		Run(control,
		    R"SQL(SELECT CASE WHEN status<>200 THEN error(decode(body)) WHEN NOT contains(decode(body),'43127') THEN error('HTTP result missing') END FROM quackapi_request('POST','/metrics-audit','{"sql":"SELECT 43127 AS http_origin_probe"}'))SQL");
		Run(control, R"SQL(WITH events AS (
 SELECT query_id,parse_duckdb_log_message('Metrics',message) AS event FROM duckdb_logs WHERE type='Metrics'
), submissions AS (
 SELECT query_id,max(event.value) FILTER(WHERE event.metric='request.sql') AS sql,
 max(event.value) FILTER(WHERE event.metric='request.origin') AS origin FROM events GROUP BY query_id
)
SELECT CASE WHEN count_if(sql='SELECT 43126 AS scalar_origin_probe' AND origin='selfdispatch')<>1
 THEN error('native scalar origin missing') END,
 CASE WHEN count_if(sql='SELECT 43127 AS http_origin_probe' AND origin='quackapi')<>1
 THEN error('HTTP origin missing') END FROM submissions)SQL");
		Run(control, "SELECT quackapi_selfdispatch('SELEC malformed_submission_probe')");
		Run(control, "SELECT CASE WHEN count(message)<>1 THEN error('parser submission missing') END FROM duckdb_logs "
		             "WHERE type='Metrics' AND parse_duckdb_log_message('Metrics',message).metric='request.sql' AND "
		             "parse_duckdb_log_message('Metrics',message).value='SELEC malformed_submission_probe'");
		Run(control, "SELECT CASE WHEN count(message)<1 THEN error('parser error missing') END FROM duckdb_logs WHERE "
		             "type='Metrics' AND parse_duckdb_log_message('Metrics',message).metric='request.error' AND "
		             "contains(parse_duckdb_log_message('Metrics',message).value,'Parser Error')");
		Run(control, R"SQL(WITH events AS (
 SELECT connection_id,query_id,parse_duckdb_log_message('Metrics',message) AS event FROM duckdb_logs WHERE type='Metrics'
)
SELECT CASE WHEN count(child.query_id)<>2 THEN error('exact native parent lineage missing') END
FROM events child JOIN events link ON child.query_id=link.query_id AND child.connection_id=link.connection_id
JOIN events parent ON parent.query_id=try_cast(link.event.value AS UBIGINT)
WHERE child.event.metric='statement.sql' AND child.event.value IN ('SELECT 43126 AS scalar_origin_probe','SELECT 43124 AS nested_metrics_probe')
AND link.event.metric='statement.parent_query_id' AND parent.event.metric='statement.sql'
AND contains(parent.event.value,child.event.value))SQL");
		Run(control, R"SQL(WITH events AS (
 SELECT connection_id,query_id,parse_duckdb_log_message('Metrics',message) AS event FROM duckdb_logs WHERE type='Metrics'
)
SELECT CASE WHEN count(child.query_id)<>2 THEN error('native submission identity missing') END
FROM events child JOIN events link ON child.query_id=link.query_id AND child.connection_id=link.connection_id
JOIN events submitted ON submitted.query_id=try_cast(link.event.value AS UBIGINT) AND submitted.connection_id=child.connection_id
WHERE child.event.metric='statement.sql' AND child.event.value IN ('SELECT 43126 AS scalar_origin_probe','SELECT 43124 AS nested_metrics_probe')
AND link.event.metric='statement.submission_id' AND submitted.event.metric='request.sql'
AND submitted.event.value=child.event.value)SQL");
		{
			Connection reused(db);
			auto stale =
			    reused.context->registered_state->GetOrCreate<QuackapiSqlSubmissionState>("quackapi.sql_submission");
			stale->parent_query_id = 123;
			stale->parent_connection_id = 456;
			stale->origin = "stale";
			Run(reused, "SELECT 43128 AS inactive_submission_probe");
		}
		Run(control, R"SQL(WITH events AS (
 SELECT connection_id,query_id,parse_duckdb_log_message('Metrics',message) AS event FROM duckdb_logs WHERE type='Metrics'
)
SELECT CASE WHEN count(stale.query_id)<>0 THEN error('inactive submission leaked provenance') END
FROM events statement JOIN events stale ON statement.query_id=stale.query_id AND statement.connection_id=stale.connection_id
WHERE statement.event.metric='statement.sql' AND statement.event.value='SELECT 43128 AS inactive_submission_probe'
AND stale.event.metric IN ('statement.parent_query_id','statement.parent_connection_id','statement.origin','statement.submission_id'))SQL");
		Run(control, R"SQL(SELECT quackapi_selfdispatch($program$-- duckstack:tag_probe
SELECT 43129 AS tagged_probe$program$).status)SQL");
		if (argc == 3) {
			string target;
			for (char ch : string(argv[2])) {
				target += ch;
				if (ch == '\'')
					target += ch;
			}
			Run(control, "COPY (SELECT "
			             "context_id,scope,connection_id,transaction_id,query_id,thread_id,timestamp,type,log_level,"
			             "message FROM duckdb_logs) TO '" +
			                 target + "' (DELIMITER '\\t', HEADER false)");
		}
		std::cout << "PASS: existing/new connections, self-dispatch, binder errors, stable query IDs" << std::endl;
		return 0;
	} catch (const std::exception &e) {
		std::cerr << e.what() << std::endl;
		return 1;
	}
}
