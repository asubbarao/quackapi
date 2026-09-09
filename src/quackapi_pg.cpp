#include "quackapi_pg.hpp"

#include "duckdb/common/string_util.hpp"
#include "quackapi_util.hpp"
#include "quackapi_limits.hpp"
#include "duckdb/parser/parser.hpp"
#include <algorithm>

#ifndef QUACKAPI_HAS_LIBPQ
// Built without libpq — always fall through to DuckDB.
namespace duckdb {
QuackapiPgNativeResult QuackapiTryPgNative(const string &, const string &,
                                           const case_insensitive_map_t<std::pair<string, string>> &, idx_t, string &,
                                           string &) {
	return QuackapiPgNativeResult::NOT_APPLICABLE;
}
} // namespace duckdb
#else

#include <libpq-fe.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <limits>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace duckdb {

namespace {

// Thread-local connection: one PGconn per HTTP worker thread (psycopg-pool shape).
//
// Do NOT cache PQprepare by SQL alone: the named statement embeds catalog OIDs
// (domains, types, …). DROP + CREATE of the same name leaves a sticky dead OID
// ("cache lookup failed for type") while the SQL text is unchanged. Mirror the
// DuckDB handler path — reuse the connection, execute fresh each request.
struct TlsPg {
	string dsn;
	PGconn *conn = nullptr;
	string err;
	//! Cached, rounded-down PostgreSQL statement_timeout. Reusing it avoids a
	//! second network round trip on the normal same-budget request path.
	int64_t statement_timeout_ms = -1;
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
		statement_timeout_ms = -1;
	}
	PGconn *Get(const string &want_dsn);
};

bool DeadlineExpired() {
	auto deadline = QuackapiCurrentDeadline();
	return deadline != std::chrono::steady_clock::time_point::max() && std::chrono::steady_clock::now() >= deadline;
}

//! Wait on the libpq socket without ever exceeding the HTTP request's shared
//! deadline. This covers connection polling, output flush, and result input.
bool WaitForPgSocket(PGconn *conn, bool want_read, bool want_write, string &err_out) {
	const int fd = PQsocket(conn);
	if (fd < 0) {
		err_out = "PostgreSQL transport failed";
		return false;
	}
	while (true) {
		auto deadline = QuackapiCurrentDeadline();
		int64_t timeout_ms = 30000;
#ifdef _WIN32
		timeval timeout {};
#endif
		if (deadline == std::chrono::steady_clock::time_point::max()) {
			// Keep the finite fallback for callers outside an HTTP request.
			timeout_ms = 30000;
#ifdef _WIN32
			timeout.tv_sec = 30;
#endif
		} else {
			auto remaining =
			    std::chrono::duration_cast<std::chrono::microseconds>(deadline - std::chrono::steady_clock::now())
			        .count();
			if (remaining <= 0) {
				err_out = "PostgreSQL deadline exceeded";
				return false;
			}
			timeout_ms = std::max<int64_t>(1, (remaining + 999) / 1000);
#ifdef _WIN32
			timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(remaining / 1000000);
			timeout.tv_usec = static_cast<decltype(timeout.tv_usec)>(remaining % 1000000);
#endif
		}

#ifdef _WIN32
		fd_set read_fds, write_fds;
		FD_ZERO(&read_fds);
		FD_ZERO(&write_fds);
		if (want_read) {
			FD_SET(fd, &read_fds);
		}
		if (want_write) {
			FD_SET(fd, &write_fds);
		}
		const int rc = select(0, want_read ? &read_fds : nullptr, want_write ? &write_fds : nullptr, nullptr, &timeout);
#else
		// poll has no FD_SETSIZE ceiling, unlike select/FD_SET. The HTTP process
		// can legitimately hold unrelated descriptors in addition to its fixed
		// worker connections, so this must remain safe at high descriptor values.
		pollfd poll_fd {};
		poll_fd.fd = fd;
		poll_fd.events = static_cast<short>((want_read ? POLLIN : 0) | (want_write ? POLLOUT : 0));
		const int bounded_timeout = static_cast<int>(std::min<int64_t>(timeout_ms, std::numeric_limits<int>::max()));
		const int rc = poll(&poll_fd, 1, bounded_timeout);
		if (rc > 0 && (poll_fd.revents & (POLLERR | POLLHUP | POLLNVAL))) {
			err_out = "PostgreSQL transport failed";
			return false;
		}
#endif
		if (rc > 0) {
			return true;
		}
		if (rc == 0) {
			err_out = "PostgreSQL deadline exceeded";
			return false;
		}
#ifndef _WIN32
		if (errno == EINTR) {
			continue;
		}
#endif
		err_out = "PostgreSQL transport failed";
		return false;
	}
}

PGconn *TlsPg::Get(const string &want_dsn) {
	if (conn && dsn == want_dsn && PQstatus(conn) == CONNECTION_OK) {
		return conn;
	}
	Reset();
	dsn = want_dsn;
	const char *keys[] = {"dbname", nullptr};
	const char *values[] = {dsn.c_str(), nullptr};
	conn = PQconnectStartParams(keys, values, 1);
	if (!conn) {
		err = "PostgreSQL connection failed";
		return nullptr;
	}
	while (true) {
		auto status = PQconnectPoll(conn);
		if (status == PGRES_POLLING_OK) {
			if (PQsetnonblocking(conn, 1) == 0) {
				return conn;
			}
			err = "PostgreSQL transport failed";
			Reset();
			return nullptr;
		}
		if (status == PGRES_POLLING_FAILED) {
			err = DeadlineExpired() ? "PostgreSQL deadline exceeded" : "PostgreSQL connection failed";
			Reset();
			return nullptr;
		}
		const bool wants_read = status == PGRES_POLLING_READING || status == PGRES_POLLING_ACTIVE;
		const bool wants_write = status == PGRES_POLLING_WRITING || status == PGRES_POLLING_ACTIVE;
		if ((!wants_read && !wants_write) || !WaitForPgSocket(conn, wants_read, wants_write, err)) {
			if (err.empty()) {
				err = "PostgreSQL transport failed";
			}
			Reset();
			return nullptr;
		}
	}
}

TlsPg &Tls() {
	thread_local TlsPg t;
	return t;
}

// $name or $name::TYPE → collect unique names in order of first appearance.
void CollectNamedParams(const string &sql, const vector<bool> &protected_text, vector<string> &names) {
	names.clear();
	for (idx_t i = 0; i < sql.size(); i++) {
		if (protected_text[i] || sql[i] != '$') {
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
	// The lexer owns quoting, dollar-quoted bodies and comments. Rewriting
	// their contents corrupts SQL literals and can invent phantom parameters.
	vector<bool> protected_text(in.size(), false);
	string executable = in;
	auto tokens = Parser::Tokenize(in);
	for (idx_t t = 0; t < tokens.size(); t++) {
		const auto &token = tokens[t];
		if (token.type == SimplifiedTokenType::SIMPLIFIED_TOKEN_STRING_CONSTANT ||
		    token.type == SimplifiedTokenType::SIMPLIFIED_TOKEN_COMMENT ||
		    (token.start < in.size() && in[token.start] == '"')) {
			auto end = t + 1 < tokens.size() ? tokens[t + 1].start : in.size();
			for (idx_t p = token.start; p < end; p++) {
				protected_text[p] = true;
				executable[p] = ' ';
			}
		}
	}
	auto lower = StringUtil::Lower(executable);
	if (StringUtil::Contains(lower, "postgres_execute") || StringUtil::Contains(lower, "postgres_query") ||
	    StringUtil::Contains(lower, "postgres_scan")) {
		err = "duckdb-only TVF";
		return false;
	}
	CollectNamedParams(in, protected_text, param_names);
	// Map name → $n
	case_insensitive_map_t<idx_t> idx;
	for (idx_t i = 0; i < param_names.size(); i++) {
		idx[param_names[i]] = i + 1;
	}
	out.clear();
	out.reserve(in.size() + 8);
	for (idx_t i = 0; i < in.size(); i++) {
		if (protected_text[i]) {
			out.push_back(in[i]);
			continue;
		}
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
			// Keep explicit casts: they determine parameter and response types.
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

//! Append while enforcing the configured final JSON body ceiling. This guards
//! each intermediate append instead of discovering an oversized body only
//! after libpq rows have been fully serialized.
bool AppendJson(string &body, const char *data, idx_t length, idx_t max_bytes, string &err_out) {
	if (DeadlineExpired()) {
		err_out = "PostgreSQL deadline exceeded";
		return false;
	}
	if (length > max_bytes || body.size() > max_bytes - length) {
		err_out = "PostgreSQL response exceeds configured byte limit";
		return false;
	}
	body.append(data, length);
	return true;
}

bool AppendJsonLiteral(string &body, const char *literal, idx_t max_bytes, string &err_out) {
	return AppendJson(body, literal, std::strlen(literal), max_bytes, err_out);
}

//! Escape directly into the bounded response rather than allocating a second
//! unbounded escaped copy of an individual PostgreSQL field.
bool AppendJsonQuoted(string &body, const char *data, idx_t length, idx_t max_bytes, string &err_out) {
	if (!AppendJsonLiteral(body, "\"", max_bytes, err_out)) {
		return false;
	}
	static const char hex[] = "0123456789abcdef";
	for (idx_t i = 0; i < length; i++) {
		if ((i & 4095) == 0 && DeadlineExpired()) {
			err_out = "PostgreSQL deadline exceeded";
			return false;
		}
		const auto ch = static_cast<unsigned char>(data[i]);
		if (ch == '"' || ch == '\\') {
			const char escaped[] = {'\\', static_cast<char>(ch)};
			if (!AppendJson(body, escaped, sizeof(escaped), max_bytes, err_out)) {
				return false;
			}
		} else if (ch == '\b') {
			if (!AppendJsonLiteral(body, "\\b", max_bytes, err_out))
				return false;
		} else if (ch == '\f') {
			if (!AppendJsonLiteral(body, "\\f", max_bytes, err_out))
				return false;
		} else if (ch == '\n') {
			if (!AppendJsonLiteral(body, "\\n", max_bytes, err_out))
				return false;
		} else if (ch == '\r') {
			if (!AppendJsonLiteral(body, "\\r", max_bytes, err_out))
				return false;
		} else if (ch == '\t') {
			if (!AppendJsonLiteral(body, "\\t", max_bytes, err_out))
				return false;
		} else if (ch < 0x20) {
			const char escaped[] = {'\\', 'u', '0', '0', hex[ch >> 4], hex[ch & 0x0F]};
			if (!AppendJson(body, escaped, sizeof(escaped), max_bytes, err_out)) {
				return false;
			}
		} else {
			const char plain[] = {static_cast<char>(ch)};
			if (!AppendJson(body, plain, sizeof(plain), max_bytes, err_out)) {
				return false;
			}
		}
	}
	return AppendJsonLiteral(body, "\"", max_bytes, err_out);
}

bool EqualsAsciiIgnoreCase(const char *value, idx_t length, const char *expected) {
	const auto expected_length = std::strlen(expected);
	if (length != expected_length) {
		return false;
	}
	for (idx_t i = 0; i < length; i++) {
		auto lhs = static_cast<unsigned char>(value[i]);
		auto rhs = static_cast<unsigned char>(expected[i]);
		if (lhs >= 'A' && lhs <= 'Z')
			lhs = static_cast<unsigned char>(lhs + ('a' - 'A'));
		if (rhs >= 'A' && rhs <= 'Z')
			rhs = static_cast<unsigned char>(rhs + ('a' - 'A'));
		if (lhs != rhs)
			return false;
	}
	return true;
}

bool AppendPgRowsJson(PGresult *res, string &body, bool &first_row, idx_t max_bytes, string &err_out) {
	const int rows = PQntuples(res);
	const int cols = PQnfields(res);
	for (int row = 0; row < rows; row++) {
		if (!first_row && !AppendJsonLiteral(body, ",", max_bytes, err_out)) {
			return false;
		}
		first_row = false;
		if (!AppendJsonLiteral(body, "{", max_bytes, err_out)) {
			return false;
		}
		for (int col = 0; col < cols; col++) {
			if (col && !AppendJsonLiteral(body, ",", max_bytes, err_out)) {
				return false;
			}
			const char *name = PQfname(res, col);
			if (!AppendJsonQuoted(body, name ? name : "", name ? std::strlen(name) : 0, max_bytes, err_out) ||
			    !AppendJsonLiteral(body, ":", max_bytes, err_out)) {
				return false;
			}
			if (PQgetisnull(res, row, col)) {
				if (!AppendJsonLiteral(body, "null", max_bytes, err_out))
					return false;
				continue;
			}
			const char *value = PQgetvalue(res, row, col);
			const idx_t length = static_cast<idx_t>(PQgetlength(res, row, col));
			const auto oid = PQftype(res, col);
			const bool numeric =
			    oid == 20 || oid == 21 || oid == 23 || oid == 26 || oid == 700 || oid == 701 || oid == 1700;
			const bool non_finite = EqualsAsciiIgnoreCase(value, length, "nan") ||
			                        EqualsAsciiIgnoreCase(value, length, "infinity") ||
			                        EqualsAsciiIgnoreCase(value, length, "-infinity");
			if (oid == 16) {
				if (!AppendJsonLiteral(body, (length == 1 && (value[0] == 't' || value[0] == 'T')) ? "true" : "false",
				                       max_bytes, err_out))
					return false;
			} else if (oid == 114 || oid == 3802 || (numeric && !non_finite)) {
				if (!AppendJson(body, value, length, max_bytes, err_out))
					return false;
			} else if (!AppendJsonQuoted(body, value, length, max_bytes, err_out)) {
				return false;
			}
		}
		if (!AppendJsonLiteral(body, "}", max_bytes, err_out)) {
			return false;
		}
	}
	return true;
}

bool IsPgCancellation(PGresult *result) {
	const char *state = PQresultErrorField(result, PG_DIAG_SQLSTATE);
	return state && std::strcmp(state, "57014") == 0;
}

bool FlushPgOutput(PGconn *conn, string &err_out) {
	while (true) {
		const auto status = PQflush(conn);
		if (status == 0) {
			return true;
		}
		if (status < 0) {
			err_out = "PostgreSQL transport failed";
			return false;
		}
		if (!WaitForPgSocket(conn, false, true, err_out)) {
			return false;
		}
	}
}

//! Consume a single non-row PostgreSQL command without a synchronous PQexec.
bool ReceivePgCommand(PGconn *conn, string &err_out) {
	if (!FlushPgOutput(conn, err_out)) {
		return false;
	}
	bool saw_command = false;
	while (true) {
		if (PQconsumeInput(conn) == 0) {
			err_out = "PostgreSQL transport failed";
			return false;
		}
		while (!PQisBusy(conn)) {
			auto *result = PQgetResult(conn);
			if (!result) {
				if (!saw_command)
					err_out = "PostgreSQL execution failed";
				return saw_command;
			}
			auto status = PQresultStatus(result);
			const bool ok = status == PGRES_COMMAND_OK;
			const bool canceled = IsPgCancellation(result);
			PQclear(result);
			if (!ok) {
				err_out =
				    canceled || DeadlineExpired() ? "PostgreSQL deadline exceeded" : "PostgreSQL execution failed";
				return false;
			}
			saw_command = true;
		}
		if (!WaitForPgSocket(conn, true, false, err_out)) {
			return false;
		}
	}
}

bool EnsurePgStatementTimeout(TlsPg &tls, PGconn *conn, string &err_out) {
	// Round down so the server-side deadline never extends the shared request
	// budget. A stable bucket avoids a SET round trip on common requests.
	const auto remaining = QuackapiRemainingTimeoutMillis(30000);
	const int64_t target = std::max<int64_t>(1, (remaining / 10) * 10);
	if (tls.statement_timeout_ms == target) {
		return true;
	}
	const string sql = "SET statement_timeout = " + std::to_string(target);
	if (PQsendQuery(conn, sql.c_str()) == 0 || !ReceivePgCommand(conn, err_out)) {
		if (err_out.empty())
			err_out = "PostgreSQL transport failed";
		return false;
	}
	tls.statement_timeout_ms = target;
	return true;
}

} // namespace

QuackapiPgNativeResult QuackapiTryPgNative(const string &dsn, const string &handler_sql,
                                           const case_insensitive_map_t<std::pair<string, string>> &provided,
                                           idx_t max_response_bytes, string &json_body, string &err_out) {
	if (dsn.empty()) {
		return QuackapiPgNativeResult::NOT_APPLICABLE;
	}
	string pg_sql;
	vector<string> names;
	if (!ToPgSql(handler_sql, pg_sql, names, err_out)) {
		// Translation happens before any network operation, so DuckDB may safely
		// handle its own syntax/parameter validation when this path cannot.
		return QuackapiPgNativeResult::NOT_APPLICABLE;
	}
	vector<const char *> vals;
	vector<string> storage;
	storage.reserve(names.size());
	vals.reserve(names.size());
	for (auto &name : names) {
		auto it = provided.find(name);
		if (it == provided.end()) {
			// Let DuckDB's normal binder produce the usual 422 missing-parameter
			// response. PostgreSQL has not observed a command at this point.
			return QuackapiPgNativeResult::NOT_APPLICABLE;
		}
		storage.push_back(it->second.second);
		vals.push_back(storage.back().c_str());
	}
	auto &tls = Tls();
	PGconn *conn = tls.Get(dsn);
	if (!conn) {
		err_out = tls.err.empty() ? "PostgreSQL connection failed" : tls.err;
		return QuackapiPgNativeResult::FAILED;
	}
	if (!EnsurePgStatementTimeout(tls, conn, err_out)) {
		tls.Reset();
		return QuackapiPgNativeResult::FAILED;
	}

	// Single-row mode means libpq yields each tuple as it arrives. We serialize
	// and cap it incrementally instead of allowing PQexecParams to accumulate an
	// unbounded full result before the HTTP response limit can be checked.
	if (PQsendQueryParams(conn, pg_sql.c_str(), static_cast<int>(vals.size()), nullptr, vals.data(), nullptr, nullptr,
	                      0) == 0 ||
	    PQsetSingleRowMode(conn) == 0) {
		err_out = "PostgreSQL transport failed";
		tls.Reset();
		return QuackapiPgNativeResult::FAILED;
	}
	if (!FlushPgOutput(conn, err_out)) {
		tls.Reset();
		return QuackapiPgNativeResult::FAILED;
	}

	json_body.clear();
	if (!AppendJsonLiteral(json_body, "[", max_response_bytes, err_out)) {
		tls.Reset();
		return QuackapiPgNativeResult::FAILED;
	}
	bool first_row = true;
	while (true) {
		if (PQconsumeInput(conn) == 0) {
			err_out = "PostgreSQL transport failed";
			tls.Reset();
			return QuackapiPgNativeResult::FAILED;
		}
		while (!PQisBusy(conn)) {
			auto *result = PQgetResult(conn);
			if (!result) {
				if (!AppendJsonLiteral(json_body, "]", max_response_bytes, err_out)) {
					tls.Reset();
					return QuackapiPgNativeResult::FAILED;
				}
				return QuackapiPgNativeResult::SUCCESS;
			}
			const auto status = PQresultStatus(result);
			if (status == PGRES_SINGLE_TUPLE) {
				const bool appended = AppendPgRowsJson(result, json_body, first_row, max_response_bytes, err_out);
				PQclear(result);
				if (!appended) {
					tls.Reset();
					return QuackapiPgNativeResult::FAILED;
				}
				continue;
			}
			const bool ok = status == PGRES_TUPLES_OK || status == PGRES_COMMAND_OK;
			const bool canceled = IsPgCancellation(result);
			PQclear(result);
			if (!ok) {
				err_out =
				    canceled || DeadlineExpired() ? "PostgreSQL deadline exceeded" : "PostgreSQL execution failed";
				tls.Reset();
				return QuackapiPgNativeResult::FAILED;
			}
		}
		if (!WaitForPgSocket(conn, true, false, err_out)) {
			tls.Reset();
			return QuackapiPgNativeResult::FAILED;
		}
	}
}

} // namespace duckdb

#endif // QUACKAPI_HAS_LIBPQ
