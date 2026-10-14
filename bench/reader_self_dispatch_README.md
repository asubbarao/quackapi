# Real file-reader self-dispatch benchmark

These SQL programs exercise one DuckDB instance calling its own QuackAPI listener. The CLI loads the chosen provider and QuackAPI artifacts, then the program starts `quackapi_serve(block := false)`, generates one literal reader query per real source file, calls `quackapi_post` on each row, consumes the complete response bodies, and stops the listener. There is no synthetic echo route, second client database, digest route, Python data script, or duplicate-path multiplier.

The source tree is the checked-out DuckDB submodule at `069cc9f9` under `READER_BENCH_SOURCE_ROOT`; both artifacts used that same tree. The file program deterministically takes the first 4,097 eligible distinct paths in sorted order from `duckdb/{src,test,tools,extension}`, excluding `third_party`, empty files, files over 128 KiB, and formats outside C/C++, Python, SQL, Markdown, JSON, YAML, and CSV. Ordinals select 3,073 `read_text` and 1,024 `read_blob` queries. The AST program takes the first 256 1–32 KiB C/C++ files under `duckdb/src` and returns every `read_ast` row and field. Each route runs `SELECT * FROM query($sql)`; every POST returns its full JSON payload to the caller. `reader_self_dispatch_verify_files.sql` compares all returned contents to direct `read_blob` bytes (including exact BLOB escaping); `reader_self_dispatch_verify_ast.sql` compares each entire JSON AST array to direct `to_json(list(r ORDER BY node_id))`. `reader_self_dispatch_verify_structured.sql` does the same for CSV, JSON, and YAML readers.

The tested artifacts were DuckDB `v1.5.6`/`069cc9f9`, baseline QuackAPI `6201b74`, cleanup QuackAPI `11440ba`, and patched `curl_httpfs` `v0.4.5` from combined source `a48f83c` (artifact SHA-256 `3e2b18e5a3a359eeea58809e7404dea3775ebb2e92e38a64271789c5b127d3a0`). Load the explicit QuackAPI and provider extension files before each program. For example, from this repository root:

```text
READER_BENCH_SOURCE_ROOT=/absolute/path/to/quackapi-checkout READER_BENCH_PORT=18884 READER_BENCH_PROFILE=/tmp/quackapi-reader-ast-profile.json /absolute/path/to/duckdb -unsigned -bail -noheader -list -cmd "LOAD '/absolute/path/to/curl_httpfs.duckdb_extension';" -cmd "LOAD '/absolute/path/to/quackapi.duckdb_extension';" -cmd "SET quackapi_post_concurrency=32;" -f bench/reader_self_dispatch_ast.sql
```

Omit the last `-cmd` for baseline, whose POST fanout is hard-coded to 32. Use a free isolated port for each process; these programs never touch the selected dev server. Repeat with `reader_self_dispatch_files.sql` and with the three verification files. The transport-only diagnostic also needs `READER_BENCH_MEMORY=1GB` (or another explicit bound). Do not use `-no-init`: normal local resource settings are loaded, then these programs explicitly set four threads, 4 GB memory (8 GB for the full AST comparison), insertion order off, 64 inbound workers, and MultiCurl. The baseline server applies its own defaults during `quackapi_serve`; the programs set `curl_httpfs_client_implementation='multi_curl'` afterward and print the resulting provider identity. That name confirms the registered provider but does not make baseline's internal HTTP path identical to cleanup's provider-backed path.

On the pinned source tree, both builds returned identical results without HTTP/provider errors:

| Program | Distinct files / POSTs | Source bytes | Returned rows | Full response-body bytes | Exact direct-reader comparison |
| --- | ---: | ---: | ---: | ---: | --- |
| Text/BLOB | 4,097 | 24,279,471 | 4,097 | 27,336,685 | 4,097/4,097 files; 0 mismatches |
| AST | 256 | 2,015,099 | 541,619 | 293,703,282 | 256/256 arrays; 0 mismatches |
| CSV | 1 | — | 109 | 8,279 | exact |
| JSON | 1 | — | 1 | 1,196 | exact |
| YAML | 1 | — | 1 | 16,731 | exact |

Representative inputs are `duckdb/extension/ExtensionDistribution.md` (2,939 bytes, beginning `## Extension served via URLs`), `duckdb/test/sakila/data/country.csv` (109 data rows), `duckdb/src/storage/version_map.json`, `duckdb/api_spec/v1/query/query.yaml`, and `duckdb/src/catalog/catalog_entry.cpp` (3,606 bytes). The AST for the latter includes a `CatalogEntry` function-definition row with semantic type `DEFINITION_FUNCTION`, three typed parameters (`CatalogType`, `string`, `idx_t`), a qualified-name STRUCT array, and a nested scope stack. A full 447,201-byte AST JSON body for `src/quackapi_logging.cpp` separately matched direct `to_json(list(r ORDER BY node_id))` byte for byte.

Timing is the DuckDB JSON profiler's **single dispatch SQL statement** latency, which includes file inventory, POSTs, complete body transfer, and `json_array_length` reduction; it excludes extension loading, route setup, and stop. One warmup and five interleaved measured runs per build/program were used on the same machine, with no concurrent build or benchmark. The maximum of five is shown instead of estimating a tail percentile from five samples:

| Program | Baseline median / max | Cleanup median / max | Baseline / cleanup peak DuckDB buffer |
| --- | ---: | ---: | ---: |
| 4,097 text/BLOB | 0.565 / 0.598 s | 0.570 / 0.590 s | ~262 / ~239 MB |
| 256 AST | 1.371 / 1.415 s | 0.628 / 0.661 s | ~2,839 / ~2,839 MB |

The file-query difference is within a few milliseconds on this local run. The AST median improved by about 2.18 times with cleanup. Separately, host-observed whole CLI command times (startup and teardown included, five measured runs) were baseline/cleanup 0.489/0.526 s for files and 1.656/0.717 s for AST. These numbers are local observations, not CI performance guarantees.

The 256-file AST query materializes 293.7 MB of response bodies. At a 256 MB DuckDB limit, even the transport-only `length(body)` variant fails at 242.1/244.1 MiB; at 1 GB it succeeds with a 323.6 MB peak buffer and no temp spill. The full `json_array_length(body::JSON)` reduction fails at 1 GB (945.5/953.6 MiB), while at 4 GB it succeeds with a 2.84 GB peak buffer and no temp spill. The distinction isolates response transport from JSON parsing; it does not imply that QuackAPI can spill those intermediate values to disk.

After the response-lifetime fix in QuackAPI `cc69600`, the three exact-value verification programs were rerun against that final artifact and the same pinned source/provider: all 4,097 file contents, all 256 complete AST arrays (541,619 nodes), and the CSV/JSON/YAML results still matched direct readers. The timing and memory figures above belong to the pre-fix `11440ba` artifact; the final fix was not included in those comparative timing runs.
