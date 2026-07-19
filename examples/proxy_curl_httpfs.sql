-- Example: reverse-proxy a URL through a quackapi route, with outbound I/O
-- accelerated by curl_httpfs (libcurl MultiCurl) when that extension is loaded.
--
-- Prerequisites (once):
--   INSTALL curl_httpfs FROM community;
--   INSTALL quackapi FROM community;   -- or LOAD a local build
--
-- Recommended load order:
--   LOAD curl_httpfs;   -- sets HTTPUtil to MultiCurl; embeds/loads httpfs
--   LOAD quackapi;

LOAD curl_httpfs;
LOAD quackapi;

-- Confirm the outbound client is MultiCurl (curl_httpfs default).
SELECT curl_httpfs_http_util_name() AS curl_httpfs_util;
-- After applying the quackapi proposal:
-- SELECT quackapi_http_util_name() AS quackapi_util;

-- Optional: libcurl verbose logs to stderr (proof that libcurl, not httplib, runs).
-- SET curl_httpfs_enable_verbose_logging = true;

-- Proxy route: path param :url is a full percent-encoded URL.
-- Clients call:  GET /proxy/<url-encoded https://...>
CREATE OR REPLACE ROUTE proxy GET '/proxy/:url' AS
SELECT
	$url AS requested_url,
	content AS body,
	length(content) AS bytes
FROM read_text($url);

-- Health / diagnostics route (no outbound I/O).
CREATE OR REPLACE ROUTE http_util GET '/http_util' AS
SELECT quackapi_http_util_name() AS http_util;

SELECT * FROM quackapi_serve(18080, host := '127.0.0.1');

-- Manual check (from another shell):
--   ENC=$(python3 -c "import urllib.parse; print(urllib.parse.quote('https://raw.githubusercontent.com/dentiny/duck-read-cache-fs/main/test/data/stock-exchanges.csv', safe=''))")
--   curl -sS "http://127.0.0.1:18080/proxy/${ENC}" | head -c 200
--   curl -sS http://127.0.0.1:18080/http_util
--
-- Expect http_util body like: [{"http_util":"MultiCurl"}]
