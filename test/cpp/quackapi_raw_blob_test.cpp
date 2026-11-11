// Real HTTP regression: explicit BODY BLOB never traverses text/JSON binding.
#include "duckdb.hpp"
#include "httplib.hpp"

#include <cstdio>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void Run(duckdb::Connection &con, const std::string &sql) {
	auto result = con.Query(sql);
	if (result->HasError())
		throw std::runtime_error(sql + ": " + result->GetError());
}
void Expect(bool ok, const std::string &label) {
	if (!ok)
		throw std::runtime_error("FAILED: " + label);
	printf("ok: %s\n", label.c_str());
}
void Reject(duckdb::Connection &con, const std::string &sql) {
	Expect(con.Query(sql)->HasError(), "incompatible raw declaration rejected: " + sql);
}
std::string Hash(duckdb::Connection &con, const std::string &body) {
	auto result = con.Query("SELECT sha256(?::BLOB)", duckdb::Value::BLOB_RAW(body));
	if (result->HasError())
		throw std::runtime_error(result->GetError());
	return result->Fetch()->GetValue(0, 0).GetValue<std::string>();
}
} // namespace

int main(int argc, char **argv) {
	setvbuf(stdout, nullptr, _IONBF, 0);
	try {
		duckdb::DuckDB database(nullptr);
		duckdb::Connection con(database);
		Run(con, "LOAD quackapi");
		// LOAD resolves the extension linked into THIS newly built libduckdb,
		// not a sibling worktree's installed loadable artifact.
		auto revision = con.Query(
		    "SELECT extension_version, install_mode FROM duckdb_extensions() WHERE extension_name='quackapi'");
		auto revision_chunk = revision->Fetch();
		Expect(revision_chunk && revision_chunk->GetValue(1, 0).ToString() == "STATICALLY_LINKED",
		       "current build is statically linked");
		printf("loaded revision: %s\n", revision_chunk->GetValue(0, 0).ToString().c_str());
		bool schema_test = argc == 3 && std::string(argv[1]) == "--schema-init";
		Expect(argc == 1 || schema_test, "optional signed schema dependency initialization is explicit");
		if (schema_test) {
			std::ifstream init(argv[2]);
			Expect(bool(init), "signed schema dependency initialization is readable");
			Run(con, std::string(std::istreambuf_iterator<char> {init}, std::istreambuf_iterator<char> {}));
			Run(con, "CREATE ROUTE schema_json POST '/schema-json' BODY SCHEMA "
			         "'{\"type\":\"object\",\"required\":[\"n\"],\"properties\":{\"n\":{\"type\":\"integer\"}}}' AS "
			         "SELECT $n::INTEGER AS n");
		}
		Run(con, "CREATE TABLE raw_assets(hash VARCHAR PRIMARY KEY, body BLOB NOT NULL)");
		Run(con, "CREATE ROUTE raw_put PUT '/raw/:hash' STATUS 201 ENVELOPE object "
		         "EMPTY STATUS 400 BODY '{\"error\":\"hash_mismatch\"}' BODY BLOB AS "
		         "INSERT INTO raw_assets SELECT $hash,$body WHERE sha256($body)=$hash "
		         "ON CONFLICT DO NOTHING RETURNING hash,octet_length(body) AS bytes");
		Run(con, "CREATE ROUTE raw_get GET '/raw/:hash' FORMAT binary EMPTY STATUS 404 BODY '{}' AS "
		         "SELECT body,'application/octet-stream' AS content_type FROM raw_assets WHERE hash=$hash");
		Run(con, "CREATE ROUTE spoof PUT '/spoof/:tag' FORMAT binary BODY BLOB AS SELECT $body AS body");
		Run(con, "CREATE ROUTE empty_first PUT '/empty-first' EMPTY STATUS 404 BODY BLOB AS SELECT $body AS body");
		Run(con, "CREATE ROUTE tiny PUT '/tiny' BODY BLOB WITH (max_request_bytes := 4) AS SELECT octet_length($body) "
		         "AS bytes");
		// Large stills: a route may opt above the 8 MiB default up to the server's hard limit.
		Run(con, "CREATE ROUTE big PUT '/big' FORMAT csv BODY BLOB WITH (max_request_bytes := 33554432) AS "
		         "SELECT octet_length($body) AS bytes, sha256($body) AS hash");
		Run(con, "CREATE ROUTE plain POST '/plain' BODY TYPE '{\"n\":\"INTEGER\"}' AS SELECT $n AS n");
		Run(con, "CREATE ROUTE json_text POST '/json-text' AS SELECT $n::INTEGER AS n");
		Run(con, "CREATE ROUTE form POST '/form' AS SELECT $n::INTEGER AS n");
		Run(con, "CREATE ROUTE multipart POST '/multipart' AS SELECT $n::INTEGER AS n");
		Run(con, "CREATE GROUP raw_guard WITH (prefix='/middleware')");
		Run(con,
		    "CREATE ROUTE guarded_raw PUT '/raw' GROUP raw_guard BODY BLOB AS SELECT octet_length($body) AS bytes");
		Run(con, "CREATE MIDDLEWARE raw_block BEFORE GROUP raw_guard AS SELECT false AS allow,429::INTEGER AS "
		         "status,'{\"error\":\"fixture-block\"}' AS body");
		Run(con, "CREATE GROUP raw_reader WITH (prefix='/reads-body')");
		Run(con, "CREATE ROUTE read_raw PUT '/raw' GROUP raw_reader BODY BLOB AS SELECT octet_length($body) AS bytes");
		Run(con, "CREATE MIDDLEWARE raw_body_reader BEFORE GROUP raw_reader AS SELECT ok AS allow,"
		         "CASE WHEN NOT ok THEN 422 END::INTEGER AS status,"
		         "CASE WHEN NOT ok THEN '{\"error\":\"middleware-saw-wrong-bytes\"}' END AS body FROM "
		         "(SELECT octet_length($body)=3 AND $body='\\x00\\xFF\\x41'::BLOB AS ok)");
		Run(con, "CREATE MIDDLEWARE raw_after AFTER AS SELECT true AS allow,'X-Raw-Test' AS header_name,'passed' AS "
		         "header_value");
		Run(con, "CREATE AUTH raw_key AS API_KEY");
		Run(con, "SELECT * FROM quackapi_add_api_key('raw_key','fixture-only-key','fixture-worker')");
		Run(con, "CREATE ROUTE auth_raw PUT '/auth-raw' REQUIRE raw_key "
		         "BODY BLOB AS SELECT octet_length($body) AS bytes");
		Reject(con, "CREATE ROUTE bad GET '/bad' BODY BLOB AS SELECT 1");
		Reject(con, "CREATE ROUTE bad PUT '/bad' BODY BLOB BODY TYPE '{\"n\":\"INTEGER\"}' AS SELECT 1");
		Reject(con, "CREATE ROUTE bad PUT '/bad' BODY TYPE '{\"n\":\"INTEGER\"}' BODY BLOB AS SELECT 1");
		Reject(con, "CREATE ROUTE bad PUT '/bad' BODY BLOB BODY SCHEMA '{}' AS SELECT 1");
		Reject(con, "CREATE ROUTE bad PUT '/bad' BODY SCHEMA '{}' BODY BLOB AS SELECT 1");
		Reject(con, "CREATE ROUTE bad PUT '/bad' BODY BLOB PARAM body VARCHAR HEADER 'body' AS SELECT 1");
		Reject(con, "CREATE ROUTE bad PUT '/bad' BODY BLOB AS $body");
		Reject(con, "CREATE ROUTE bad PUT '/bad/:body' BODY BLOB AS SELECT 1");
		Run(con, "SELECT * FROM quackapi_serve(9541,host:='127.0.0.1',access_log:=false,max_request_bytes:=67108864)");
		quackapi_httplib::Client http("127.0.0.1", 9541);
		http.set_read_timeout(10, 0);
		const std::vector<std::string> fixtures = {std::string("\0\xff\x41", 3), std::string("\xff\xfe\x80", 3),
		                                           R"(\x00\xFF\n\\literal)", "", std::string("1234")};
		for (const auto &bytes : fixtures) {
			auto hash = Hash(con, bytes);
			auto put =
			    http.Put("/raw/" + hash + "?body=spoof", {{"body", "header-spoof"}}, bytes, "application/octet-stream");
			Expect(put && put->status == 201, "raw PUT accepts fixture length=" + std::to_string(bytes.size()));
			auto get = http.Get("/raw/" + hash);
			Expect(get && get->status == 200 && get->body == bytes, "HTTP GET returns byte-for-byte native BLOB");
			Expect(get->get_header_value("X-Raw-Test") == "passed", "AFTER middleware still applies to binary output");
			Expect(Hash(con, get->body) == hash, "download SHA256 equals selected-byte SHA256");
			auto spoof = http.Put("/spoof/path-spoof?body=query-spoof", {{"body", "header-spoof"}}, bytes,
			                      "application/octet-stream");
			Expect(spoof && spoof->status == 200 && spoof->body == bytes,
			       "path/query/header cannot replace reserved raw body");
		}
		auto tiny_ok = http.Put("/tiny", "1234", "application/octet-stream");
		Expect(tiny_ok && tiny_ok->status == 200, "exact route size limit accepted");
		auto tiny_bad = http.Put("/tiny", "12345", "application/octet-stream");
		Expect(tiny_bad && tiny_bad->status == 413, "one byte over route size limit rejected");
		auto huge = http.Put("/raw/huge", std::string(8 * 1024 * 1024 + 1, 'x'), "application/octet-stream");
		Expect(huge && huge->status == 413, "one byte over inherited 8MiB limit rejected");
		std::string large(20 * 1024 * 1024, '\0');
		for (size_t i = 0; i < large.size(); i++) {
			large[i] = static_cast<char>((i * 2654435761u) >> 13);
		}
		auto big_ok = http.Put("/big", large, "application/octet-stream");
		Expect(big_ok && big_ok->status == 200 &&
		           big_ok->body.find(std::to_string(large.size()) + "," + Hash(con, large)) != std::string::npos,
		       "20MiB raw body above the 8MiB default arrives intact (length and SHA256) on an opted-in route");
		auto big_bad = http.Put("/big", std::string(32 * 1024 * 1024 + 1, 'x'), "application/octet-stream");
		Expect(big_bad && big_bad->status == 413, "one byte over the opted-in route limit rejected");
		for (const auto &ct : {"application/json", "text/plain", "application/x-www-form-urlencoded"}) {
			auto wrong = http.Put("/raw/wrong", "{}", ct);
			Expect(wrong && wrong->status == 415, "raw route rejects other content type");
		}
		quackapi_httplib::Request empty_request;
		empty_request.method = "PUT";
		empty_request.path = "/raw/no-ct";
		auto no_ct = http.send(empty_request);
		Expect(no_ct && no_ct->status == 415, "raw empty request still requires octet-stream");
		auto bad_hash = http.Put("/raw/incorrect", "abc", "application/octet-stream");
		Expect(bad_hash && bad_hash->status == 400, "hash mismatch never stores bytes");
		for (const auto &path : {"/plain", "/json-text"}) {
			auto wrong = http.Post(path, std::string("\0\xff\x41", 3), "application/octet-stream");
			Expect(wrong && wrong->status == 422, "non-raw JSON route still rejects octet-stream");
			auto invalid = http.Post(path, R"({"n":"bad"})", "application/json");
			Expect(invalid && invalid->status == 422, "JSON integer validation retained");
			auto valid = http.Post(path, R"({"n":7})", "application/json");
			Expect(valid && valid->status == 200, "valid JSON retained");
		}
		if (schema_test) {
			auto wrong_media = http.Post("/schema-json", std::string("\0\xff\x41", 3), "application/octet-stream");
			Expect(wrong_media && wrong_media->status == 422, "BODY SCHEMA still rejects raw bytes");
			auto invalid = http.Post("/schema-json", R"({"n":"bad"})", "application/json");
			Expect(invalid && invalid->status == 422, "BODY SCHEMA validation retained");
			auto valid = http.Post("/schema-json", R"({"n":7})", "application/json");
			Expect(valid && valid->status == 200, "BODY SCHEMA valid JSON retained");
		} else {
			printf("SKIP BODY SCHEMA HTTP checks: provide --schema-init with a signed dependency bootstrap\n");
		}
		auto blocked = http.Put("/middleware/raw", std::string("\0\xff\x41", 3), "application/octet-stream");
		Expect(blocked && blocked->status == 429, "BEFORE middleware still blocks raw routes");
		auto read_by_middleware = http.Put("/reads-body/raw", std::string("\0\xff\x41", 3), "application/octet-stream");
		Expect(read_by_middleware && read_by_middleware->status == 200,
		       "middleware reading $body sees the exact non-UTF-8 BLOB bytes");
		auto authorized = http.Put("/auth-raw", {{"X-API-Key", "fixture-only-key"}}, std::string("\0\xff\x41", 3),
		                           "application/octet-stream");
		Expect(authorized && authorized->status == 200, "authenticated raw bytes retain normal auth behavior");
		auto form = http.Post("/form", "n=7", "application/x-www-form-urlencoded");
		Expect(form && form->status == 200, "form behavior retained");
		quackapi_httplib::UploadFormDataItems parts = {{"n", "7", "", ""}};
		auto multipart = http.Post("/multipart", parts);
		Expect(multipart && multipart->status == 200, "multipart behavior retained");
		auto unauthorized = http.Put("/auth-raw", "abc", "application/octet-stream");
		Expect(unauthorized && unauthorized->status == 401, "raw body does not bypass auth");
		auto counts = con.Query("SELECT count(*) FROM raw_assets");
		Expect(counts->Fetch()->GetValue(0, 0).GetValue<int64_t>() == int64_t(fixtures.size()),
		       "only valid fixtures persisted");
		Run(con, "SELECT * FROM quackapi_stop(9541)");
	} catch (const std::exception &ex) {
		fprintf(stderr, "%s\n", ex.what());
		return 1;
	}
	return 0;
}
