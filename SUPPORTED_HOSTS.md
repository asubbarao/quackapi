# Supported DuckDB host — quackapi

**Only DuckDB v2.0.** Linux, macOS, and Windows (`windows_amd64`).

v2.0 has no `v2.0.0` tag yet: the line lives on the `v2.0-cyanoptera` release
branch, and the `duckdb` submodule pins commit `3a604ab` on it. A build of that
commit reports itself as `v2.0.0-dev<N>`.

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

| DuckDB | Linux | macOS | Windows |
|--------|-------|-------|---------|
| **v2.0** (`v2.0-cyanoptera`) | yes | yes | yes |
| anything else | not supported | not supported | not supported |

Older hosts (1.5.x, 1.4.x, …) are **out of scope**. Upgrade DuckDB (and quack) to **2.0**.

CI and GitHub Release assets are built only for **v2.0** × linux_amd64/arm64, osx_amd64/arm64, windows_amd64.
