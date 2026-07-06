-- ============================================================================
-- compose.sql — the COMPOSABILITY RECEIPTS.
-- Thesis: handlers are SQL, so any LOADed DuckDB extension composes inside a
-- request with ZERO framework changes. This file proves it: five community
-- extensions doing real API work (validation, semantic typing, signing,
-- templating, SQL linting, parallel HTTP fan-out), each registered as plain
-- route DATA. No framework edits. FastAPI's equivalent is pip + glue code
-- per feature; here each feature is `LOAD x` + an INSERT.
--
-- Additive: loads AFTER framework.sql (and optionally app.sql). Only touches
-- its own compose_* route_ids. NO regex, NO LIKE. Extensions are pre-loaded
-- here at boot — never inside a request.
-- ============================================================================

INSTALL json_schema FROM community;  LOAD json_schema;
INSTALL finetype    FROM community;  LOAD finetype;
INSTALL crypto      FROM community;  LOAD crypto;
INSTALL tera        FROM community;  LOAD tera;
INSTALL parser_tools FROM community; LOAD parser_tools;
INSTALL curl_httpfs FROM community;  LOAD curl_httpfs;

-- Idempotency: remove this file's own registrations (set-based, prefix-scoped).
DELETE FROM param_schema WHERE starts_with(route_id, 'compose_');
DELETE FROM route_headers WHERE starts_with(route_id, 'compose_');
DELETE FROM routes WHERE starts_with(route_id, 'compose_');

-- ----------------------------------------------------------------------------
-- Config-as-data for the receipts (schemas, secrets, templates, fan-out URLs).
-- ----------------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS compose_schemas (id VARCHAR, body_schema VARCHAR);
TRUNCATE TABLE compose_schemas;
INSERT INTO compose_schemas BY NAME
SELECT 'order' AS id,
       '{"type":"object","required":["sku","qty"],"properties":{"sku":{"type":"string","minLength":3},"qty":{"type":"integer","minimum":1,"maximum":999}},"additionalProperties":false}' AS body_schema;

CREATE TABLE IF NOT EXISTS compose_secrets (name VARCHAR, secret VARCHAR);
TRUNCATE TABLE compose_secrets;
INSERT INTO compose_secrets BY NAME SELECT 'webhook' AS name, 'demo-signing-secret' AS secret;

CREATE TABLE IF NOT EXISTS compose_templates (id VARCHAR, tpl VARCHAR);
TRUNCATE TABLE compose_templates;
INSERT INTO compose_templates BY NAME
SELECT 'report' AS id,
       '<html><body><h1>Users ({{ users | length }})</h1><ul>{% for u in users %}<li>{{ u.name }} ({{ u.age }})</li>{% endfor %}</ul></body></html>' AS tpl;

CREATE TABLE IF NOT EXISTS compose_fanout_urls (url VARCHAR);
TRUNCATE TABLE compose_fanout_urls;
INSERT INTO compose_fanout_urls BY NAME
SELECT unnest([
  'https://raw.githubusercontent.com/duckdb/duckdb/main/README.md',
  'https://raw.githubusercontent.com/duckdb/community-extensions/main/README.md',
  'https://raw.githubusercontent.com/duckdb/duckdb-web/main/README.md'
]) AS url;

-- ----------------------------------------------------------------------------
-- RECEIPT 1 — json_schema: a declarative document-validation endpoint.
-- Pydantic-class validation from a stored JSON Schema, one extension function.
-- POST /check/{schema_id}  body: {"doc": "<json document as string>"}
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_check', 'POST', '/check/{schema_id}',
  'SELECT to_json({schema_id: {schema_id}, parseable: try_cast({doc} AS JSON) IS NOT NULL, valid: coalesce(try(json_schema_validate(s.body_schema, {doc})), false)}) AS body FROM compose_schemas s WHERE s.id = {schema_id}',
  'dynamic', 'Validate a JSON document against a stored JSON Schema (json_schema ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 2 — finetype: semantic type inference as an API (244 semantic types).
-- GET /classify?value=...   -> {semantic_type, detail}
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_classify', 'GET', '/classify',
  'SELECT to_json({value: {value}, semantic_type: finetype({value}), detail: try_cast(finetype_detail({value}) AS JSON)}) AS body',
  'dynamic', 'Semantic type inference for a value (finetype ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 3 — crypto: HMAC webhook signing inside the request pipeline.
-- POST /webhooks/sign   body: {"payload": "..."}  -> hex HMAC-SHA256
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_sign', 'POST', '/webhooks/sign',
  'SELECT to_json({algo: ''hmac-sha2-256'', signature: lower(hex(crypto_hmac(''sha2-256'', s.secret, {payload})))}) AS body FROM compose_secrets s WHERE s.name = ''webhook''',
  'dynamic', 'HMAC-sign a payload with a server-side secret (crypto ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 4 — tera: server-rendered HTML report over LIVE table data.
-- GET /report  -> Jinja2-class template, rendered in-engine next to the data.
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_report', 'GET', '/report',
  'SELECT tera_render(t.tpl, to_json({users: (SELECT coalesce(json_group_array(to_json(u)), ''[]''::JSON) FROM users u)})) AS body FROM compose_templates t WHERE t.id = ''report''',
  'dynamic', 'HTML report rendered by tera over live users table (tera ext; served application/json pending a dynamic-html kind — see COMPOSABILITY.md gaps)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 5 — parser_tools: a SQL-linting endpoint.
-- POST /sql/lint  body: {"q": "<sql>"}  -> {parseable: bool}
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_lint', 'POST', '/sql/lint',
  'SELECT to_json({parseable: is_parsable({q})}) AS body',
  'dynamic', 'Lint a SQL string with the engine''s own parser (parser_tools ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 6 — curl_httpfs: PARALLEL HTTP fan-out inside one request.
-- GET /fanout  -> fetches N upstream URLs concurrently (pooled HTTP/2), returns
-- per-URL byte counts. FastAPI needs an async client + gather; here it is one
-- table function over a URL list. URLs come from config (inlined at registration
-- below — handler templates take literal args only).
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_fanout', 'GET', '/fanout',
  'SELECT coalesce(json_group_array(to_json({url: t.filename, bytes: length(t.content)})), ''[]'') AS body FROM read_text([__URLS__]) t',
  'dynamic', 'Parallel HTTP fan-out via curl_httpfs read_text([urls])', 200);

UPDATE routes
SET handler = replace(handler, '__URLS__',
      (SELECT string_agg('''' || url || '''', ', ') FROM compose_fanout_urls))
WHERE route_id = 'compose_fanout';

-- ----------------------------------------------------------------------------
-- Param declarations — same validation pipeline (422s) guards every receipt.
-- ----------------------------------------------------------------------------
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
SELECT 'compose_check',    'schema_id', 'path',  'string', true,  NULL UNION ALL
SELECT 'compose_check',    'doc',       'body',  'string', true,  NULL UNION ALL
SELECT 'compose_classify', 'value',     'query', 'string', true,  NULL UNION ALL
SELECT 'compose_sign',     'payload',   'body',  'string', true,  NULL UNION ALL
SELECT 'compose_lint',     'q',         'body',  'string', true,  NULL;

-- ============================================================================
-- WAVE 2 RECEIPTS — fts, cronjob, bitfilters, rapidfuzz, markdown.
-- (postgres receipt lives in compose_pg.sql: it requires a local Postgres and
--  must not break compose.sql on machines without one.)
-- ============================================================================
INSTALL fts;                            LOAD fts;
INSTALL cronjob    FROM community;      LOAD cronjob;
INSTALL bitfilters FROM community;      LOAD bitfilters;
INSTALL rapidfuzz  FROM community;      LOAD rapidfuzz;
INSTALL markdown   FROM community;      LOAD markdown;

-- Seeds ----------------------------------------------------------------------
CREATE TABLE IF NOT EXISTS compose_articles (id INTEGER, title VARCHAR, body VARCHAR);
TRUNCATE TABLE compose_articles;
INSERT INTO compose_articles BY NAME
SELECT * FROM (
  SELECT 1 AS id, 'Ducks and databases' AS title,
         'DuckDB is an in-process analytical database that quacks SQL fluently.' AS body
  UNION ALL SELECT 2, 'Feeding your flock',
         'A balanced duck feed keeps the pond ecosystem healthy through winter.'
  UNION ALL SELECT 3, 'HTTP servers in strange places',
         'You can serve HTTP from almost anything, including a database engine.'
  UNION ALL SELECT 4, 'Winter pond maintenance',
         'Liners crack in deep cold; inspect the pond edges before first frost.'
);
PRAGMA create_fts_index('compose_articles', 'id', 'title', 'body', overwrite=1);

CREATE TABLE IF NOT EXISTS compose_allowlist (key VARCHAR);
TRUNCATE TABLE compose_allowlist;
INSERT INTO compose_allowlist BY NAME
SELECT unnest(['key-alpha','key-bravo','key-charlie']) AS key;

CREATE TABLE IF NOT EXISTS compose_heartbeats (ts TIMESTAMP);

-- ----------------------------------------------------------------------------
-- RECEIPT 7 — fts (core): BM25-ranked full-text search endpoint.
-- FastAPI's answer to "add search" is an external engine. Here: one PRAGMA.
-- GET /articles/search?q=...
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_search_fts', 'GET', '/articles/search',
  'SELECT coalesce(json_group_array(to_json(t)), ''[]'') AS body FROM (SELECT a.id, a.title, round(s.score, 3) AS score FROM (SELECT id, fts_main_compose_articles.match_bm25(id, {q}) AS score FROM compose_articles) s JOIN compose_articles a USING (id) WHERE s.score IS NOT NULL ORDER BY s.score DESC LIMIT 5) t',
  'dynamic', 'BM25 full-text search over articles (fts ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 8 — cronjob: background/scheduled jobs, the Celery slot.
-- Defeats edges.md hypothesis #4 (fire-and-forget after response) by extension.
-- POST /jobs/heartbeat {schedule} registers; GET /jobs lists live jobs.
-- One-shot Tier-1 sessions can register+list; firing is proven by
-- test/compose_cron_fire.sh (a >60s live session).
--
-- WEIGHT NOTE: at 24 MB this is the heaviest extension in compose.sql — kept
-- because it replaces an entire Celery + Redis + broker + worker deployment.
-- Judge extensions per-MB against what they displace, not in isolation.
--
-- ⚠ CRON SYNTAX: cron() takes SIX-field, SECONDS-FIRST expressions
-- ('*/10 * * * * *' = every 10s). The classic five-field crontab form
-- ('*/5 * * * *') is REJECTED with "must consist of 6 fields" — LLMs and
-- humans both default to five fields; don't.
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_job_add', 'POST', '/jobs/heartbeat',
  'SELECT to_json({scheduled: cron(''INSERT INTO compose_heartbeats SELECT now()'', {schedule})}) AS body',
  'dynamic', 'Schedule a recurring background job (cronjob ext)', 201);
INSERT INTO routes SELECT * FROM register_route(
  'compose_job_list', 'GET', '/jobs',
  'SELECT coalesce(json_group_array(to_json(j)), ''[]'') AS body FROM cron_jobs() j',
  'dynamic', 'List live background jobs (cronjob ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 9 — bitfilters: probabilistic membership (xor filter) endpoint.
-- The primitive under rate-limiting / dedup / "have we seen this key".
-- GET /allowlist/check?id=...
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_allow', 'GET', '/allowlist/check',
  'SELECT to_json({id: {id}, known: xor8_filter_contains(f.flt, hash({id}))}) AS body FROM (SELECT xor8_filter(hash(key)) AS flt FROM compose_allowlist) f',
  'dynamic', 'Probabilistic membership check via xor filter (bitfilters ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 10 — rapidfuzz: typo-tolerant fuzzy match endpoint.
-- GET /users/fuzzy?name=...
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_fuzzy', 'GET', '/users/fuzzy',
  'SELECT to_json(t) AS body FROM (SELECT u.name AS best_match, round(rapidfuzz_token_sort_ratio(lower(u.name), lower({name})), 1) AS score FROM users u ORDER BY score DESC LIMIT 1) t',
  'dynamic', 'Typo-tolerant fuzzy user lookup (rapidfuzz ext)', 200);

-- ----------------------------------------------------------------------------
-- RECEIPT 11 — markdown: md -> HTML rendering endpoint.
-- POST /render/md {"md": "..."}
-- ----------------------------------------------------------------------------
INSERT INTO routes SELECT * FROM register_route(
  'compose_md', 'POST', '/render/md',
  'SELECT to_json({html: md_to_html({md})}) AS body',
  'dynamic', 'Render markdown to HTML in-engine (markdown ext)', 200);

-- Param declarations -----------------------------------------------------------
INSERT INTO param_schema (route_id, name, location, type, required, constraint_json)
SELECT 'compose_search_fts', 'q',        'query', 'string', true, NULL UNION ALL
SELECT 'compose_job_add',    'schedule', 'body',  'string', true, NULL UNION ALL
SELECT 'compose_allow',      'id',       'query', 'string', true, NULL UNION ALL
SELECT 'compose_fuzzy',      'name',     'query', 'string', true, NULL UNION ALL
SELECT 'compose_md',         'md',       'body',  'string', true, NULL;
