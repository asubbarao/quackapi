-- Run from the repository root with the CLI built by this checkout:
-- ./build/release/duckdb -no-agent -unsigned -bail -json -f bench/large_response.sql
LOAD 'build/release/extension/quackapi/quackapi.duckdb_extension';
CREATE ROUTE large GET '/large' AS SELECT range AS id, repeat('x', 128) AS value FROM range(100000);
.timer on
SELECT status, octet_length(body) AS bytes FROM quackapi_request('GET', '/large', max_response_bytes := 134217728);
SELECT status, octet_length(body) AS bytes FROM quackapi_request('GET', '/large', max_response_bytes := 134217728);
SELECT status, octet_length(body) AS bytes FROM quackapi_request('GET', '/large', max_response_bytes := 134217728);
CREATE ROUTE large_arrow GET '/large-arrow' FORMAT ARROW AS SELECT range AS id, repeat('x', 128) AS value FROM range(100000);
SELECT status, octet_length(body) AS bytes FROM quackapi_request('GET', '/large-arrow', max_response_bytes := 134217728);
SELECT status, octet_length(body) AS bytes FROM quackapi_request('GET', '/large-arrow', max_response_bytes := 134217728);
SELECT status, octet_length(body) AS bytes FROM quackapi_request('GET', '/large-arrow', max_response_bytes := 134217728);
