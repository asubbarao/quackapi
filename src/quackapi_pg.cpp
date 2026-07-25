#include "quackapi_pg.hpp"

#include "duckdb/common/string_util.hpp"
#include "quackapi_util.hpp"

#ifndef QUACKAPI_HAS_LIBPQ
// Built without libpq — always fall through to DuckDB.
namespace duckdb {
bool QuackapiTryPgNative(const string &, const string &, const case_insensitive_map_t<std::pair<string, string>> &,
                         string &, string &) {
	return false;
}
} // namespace duckdb
#else

#include <libpq-fe.h>

#include <unordered_map>
#include <vector>

namespace duckdb {

namespace {

// Thread-local connection: one PGconn per HTTP worker thread (psycopg-pool shape).
// Prepared statements cached per thread (PQprepare once → PQexecPrepared).
struct TlsPg {
	string dsn;
	PGconn *conn = nullptr;
	string err;
	unordered_map<string, string> prepared; // pg_sql → stmt name
	idx_t prep_seq = 0;
	~TlsPg() {
		if (conn) {
			PQfinish(conn);
			conn = nullptr;
		}
	}
	void Reset() {
		if (conn) {
			PQfinish(conn);
			conn = nullptr;
		}
		prepared.clear();
		prep_seq = 0;
	}
	PGconn *Get(const string &want_dsn) {
		if (conn && dsn == want_dsn && PQstatus(conn) == CONNECTION_OK) {
			return conn;
		}
		Reset();
		dsn = want_dsn;
		conn = PQconnectdb(dsn.c_str());
		if (PQstatus(conn) != CONNECTION_OK) {
			err = PQerrorMessage(conn);
			PQfinish(conn);
			conn = nullptr;
			return nullptr;
		}
		return conn;
	}
	// Prepare-once execute. nparams must match SQL $1..$n.
	PGresult *ExecPrepared(const string &pg_sql, int nparams, const char *const *vals) {
		if (!conn) {
			return nullptr;
		}
		string name;
		auto it = prepared.find(pg_sql);
		if (it == prepared.end()) {
			if (prepared.size() >= 64) {
				// Drop cache (PG keeps plans on conn; names collide only after Reset).
				prepared.clear();
			}
			name = "qa" + std::to_string(prep_seq++);
			PGresult *pr = PQprepare(conn, name.c_str(), pg_sql.c_str(), nparams, nullptr);
			if (!pr || PQresultStatus(pr) != PGRES_COMMAND_OK) {
				if (pr) {
					err = PQresultErrorMessage(pr);
					PQclear(pr);
				} else {
					err = "PQprepare null";
				}
				return nullptr;
			}
			PQclear(pr);
			prepared.emplace(pg_sql, name);
		} else {
			name = it->second;
		}
		return PQexecPrepared(conn, name.c_str(), nparams, vals, nullptr, nullptr, 0);
	}
};

TlsPg &Tls() {
	thread_local TlsPg t;
	return t;
}

// $name or $name::TYPE → collect unique names in order of first appearance.
void CollectNamedParams(const string &sql, vector<string> &names) {
	names.clear();
	for (idx_t i = 0; i < sql.size(); i++) {
		if (sql[i] != '$') {
			continue;
		}
		if (i + 1 >= sql.size() || !((sql[i + 1] >= 'A' && sql[i + 1] <= 'Z') ||
		                             (sql[i + 1] >= 'a' && sql[i + 1] <= 'z') || sql[i + 1] == '_')) {
			continue;
		}
		idx_t j = i + 1;
		while (j < sql.size() && ((sql[j] >= 'A' && sql[j] <= 'Z') || (sql[j] >= 'a' && sql[j] <= 'z') ||
		                          (sql[j] >= '0' && sql[j] <= '9') || sql[j] == '_')) {
			j++;
		}
		string name = sql.substr(i + 1, j - (i + 1));
		// skip ::type
		bool seen = false;
		for (auto &n : names) {
			if (StringUtil::Lower(n) == StringUtil::Lower(name)) {
				seen = true;
				break;
			}
		}
		if (!seen) {
			names.push_back(name);
		}
		i = j - 1;
	}
}

// DuckDB route SQL → Postgres text suitable for PQexecParams.
//  - strip "pg." catalog qualifier
//  - $name::TYPE / $name → $1, $2, … (order of first appearance)
// Rejects obvious DuckDB-only surface (postgres_execute, etc.).
bool ToPgSql(const string &in, string &out, vector<string> &param_names, string &err) {
	auto lower = StringUtil::Lower(in);
	if (StringUtil::Contains(lower, "postgres_execute") || StringUtil::Contains(lower, "postgres_query") ||
	    StringUtil::Contains(lower, "postgres_scan")) {
		err = "duckdb-only TVF";
		return false;
	}
	CollectNamedParams(in, param_names);
	// Map name → $n
	case_insensitive_map_t<idx_t> idx;
	for (idx_t i = 0; i < param_names.size(); i++) {
		idx[param_names[i]] = i + 1;
	}
	out.clear();
	out.reserve(in.size() + 8);
	for (idx_t i = 0; i < in.size(); i++) {
		// strip pg. qualifier (case-insensitive)
		if ((in[i] == 'p' || in[i] == 'P') && i + 2 < in.size() && (in[i + 1] == 'g' || in[i + 1] == 'G') &&
		    in[i + 2] == '.') {
			// word boundary before p
			if (i == 0 || !((in[i - 1] >= 'A' && in[i - 1] <= 'Z') || (in[i - 1] >= 'a' && in[i - 1] <= 'z') ||
			                (in[i - 1] >= '0' && in[i - 1] <= '9') || in[i - 1] == '_')) {
				i += 2;
				continue;
			}
		}
		if (in[i] == '$' && i + 1 < in.size() &&
		    ((in[i + 1] >= 'A' && in[i + 1] <= 'Z') || (in[i + 1] >= 'a' && in[i + 1] <= 'z') || in[i + 1] == '_')) {
			idx_t j = i + 1;
			while (j < in.size() && ((in[j] >= 'A' && in[j] <= 'Z') || (in[j] >= 'a' && in[j] <= 'z') ||
			                         (in[j] >= '0' && in[j] <= '9') || in[j] == '_')) {
				j++;
			}
			string name = in.substr(i + 1, j - (i + 1));
			// skip ::TYPE
			if (j + 1 < in.size() && in[j] == ':' && in[j + 1] == ':') {
				j += 2;
				while (j < in.size() && ((in[j] >= 'A' && in[j] <= 'Z') || (in[j] >= 'a' && in[j] <= 'z') ||
				                         (in[j] >= '0' && in[j] <= '9') || in[j] == '_')) {
					j++;
				}
			}
			auto it = idx.find(name);
			if (it == idx.end()) {
				err = "unmapped param $" + name;
				return false;
			}
			out += "$";
			out += std::to_string(it->second);
			i = j - 1;
			continue;
		}
		out.push_back(in[i]);
	}
	return true;
}

string EscapeJson(const char *s) {
	return QuackapiJsonEscape(string(s ? s : ""));
}

string ResultToJson(PGresult *res) {
	int rows = PQntuples(res);
	int cols = PQnfields(res);
	string body = "[";
	for (int r = 0; r < rows; r++) {
		if (r) {
			body += ",";
		}
		body += "{";
		for (int c = 0; c < cols; c++) {
			if (c) {
				body += ",";
			}
			body += "\"";
			body += EscapeJson(PQfname(res, c));
			body += "\":";
			if (PQgetisnull(res, r, c)) {
				body += "null";
			} else {
				// JSON-encode as string or number: digits / bool / else string
				const char *v = PQgetvalue(res, r, c);
				bool num = v[0] != '\0';
				for (const char *p = v; *p; p++) {
					if (!((*p >= '0' && *p <= '9') || *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) {
						num = false;
						break;
					}
				}
				auto lower = StringUtil::Lower(string(v));
				if (lower == "true" || lower == "false") {
					body += lower;
				} else if (num) {
					body += v;
				} else {
					body += "\"";
					body += EscapeJson(v);
					body += "\"";
				}
			}
		}
		body += "}";
	}
	body += "]";
	return body;
}

} // namespace

bool QuackapiTryPgNative(const string &dsn, const string &handler_sql,
                         const case_insensitive_map_t<std::pair<string, string>> &provided, string &json_body,
                         string &err_out) {
	if (dsn.empty()) {
		return false;
	}
	string pg_sql;
	vector<string> names;
	if (!ToPgSql(handler_sql, pg_sql, names, err_out)) {
		return false;
	}
	auto &tls = Tls();
	PGconn *conn = tls.Get(dsn);
	if (!conn) {
		err_out = tls.err.empty() ? "pg connect failed" : tls.err;
		return false;
	}
	vector<const char *> vals;
	vector<string> storage;
	storage.reserve(names.size());
	vals.reserve(names.size());
	for (auto &n : names) {
		auto it = provided.find(n);
		if (it == provided.end()) {
			err_out = "missing param " + n;
			return false;
		}
		storage.push_back(it->second.second);
		vals.push_back(storage.back().c_str());
	}
	PGresult *res = tls.ExecPrepared(pg_sql, (int)vals.size(), vals.data());
	if (!res) {
		err_out = tls.err.empty() ? string("PQexecPrepared null") : tls.err;
		return false;
	}
	auto status = PQresultStatus(res);
	if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
		err_out = PQresultErrorMessage(res);
		PQclear(res);
		// drop dead connection + prepared cache
		if (PQstatus(conn) != CONNECTION_OK) {
			tls.Reset();
		}
		return false;
	}
	if (status == PGRES_COMMAND_OK) {
		// no rows — empty array
		json_body = "[]";
	} else {
		json_body = ResultToJson(res);
	}
	PQclear(res);
	return true;
}

} // namespace duckdb

#endif // QUACKAPI_HAS_LIBPQ
