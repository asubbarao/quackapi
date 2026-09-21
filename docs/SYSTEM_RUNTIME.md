# Shared system runtime

Loading QuackAPI leaves telemetry off. Operators can opt in with
`SET quackapi_otlp = 'local'` or an explicit endpoint, then inspect
`quackapi_otlp()`. This keeps an ordinary HTTP service from opening an
unrequested OTLP listener on port 4318.

The DuckDB submodule is pinned to v1.5.5. On macOS arm64, build the local
extension without a Homebrew libpq dependency:

```text
cmake -S duckdb -B build/system -G Ninja -DCMAKE_BUILD_TYPE=Release -DDUCKDB_EXTENSION_CONFIGS=/ABSOLUTE/PATH/extension_config.cmake -DBUILD_UNITTESTS=OFF -DBUILD_SHELL=OFF -DQUACKAPI_ENABLE_LIBPQ=OFF -DCMAKE_OSX_ARCHITECTURES=arm64 -DOVERRIDE_GIT_DESCRIBE=v1.5.5
cmake --build build/system --target quackapi_loadable_extension -j 4
```

Serving requires compatible `curl_httpfs` and `httpfs_timeout_retry` extensions.
Install or stage those separately; no download occurs inside a request. The
runtime smoke accepts an explicitly staged timeout companion and uses the
engine's installed curl companion:

```text
python3 test/integration/test_system_runtime.py --duckdb /opt/homebrew/bin/duckdb --extension build/system/extension/quackapi/quackapi.duckdb_extension --httpfs-timeout-retry build/dependencies/httpfs_timeout_retry.duckdb_extension
```

The test starts an isolated ephemeral DuckDB process on a selected loopback
port and cleans it up afterward. Its client always uses that port and never
replays requests. It bypasses the user's startup file and allows the explicitly
built unsigned artifact while keeping engine/extension compatibility checks.

Verified on DuckDB v1.5.5: `tune := false` retains the configured 20 GiB buffer
limit, 12 database threads, default insertion ordering, and disabled logging.
The HTTP listener reports eight workers and 32 pending requests. Sixteen
concurrent runtime reads succeed; a response exceeding the configured 8 MiB
limit returns HTTP 507, and a subsequent request succeeds. The OTLP extension
remains unloaded. The test supplies a 30,000 ms query deadline; this smoke does
not claim to exercise that deadline or the entire extension test suite.

Verified companion artifacts for this build:

| Artifact | SHA-256 |
| --- | --- |
| v1.5.5/osx_arm64/httpfs_timeout_retry.duckdb_extension | `95327ec9182429d48dc9f66f55c79e14180c65020f5e78fb4fd297a7b3f5e898` |
| v1.5.5/osx_arm64/curl_httpfs.duckdb_extension | `6d539010141dac56645ee4380b0df7f2e753d4d294a9d7b680a0b62fa9dbfab9` |

The timeout companion was downloaded from
`https://community-extensions.duckdb.org/v1.5.5/osx_arm64/httpfs_timeout_retry.duckdb_extension.gz`.
The test prints the loaded QuackAPI artifact's hash with its runtime evidence.
