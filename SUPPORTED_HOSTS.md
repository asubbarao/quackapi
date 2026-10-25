# Supported DuckDB host — quackapi

**DuckDB v2.0.0.** Linux, macOS, and Windows (`windows_amd64`).

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

| DuckDB | Linux | macOS | Windows |
|--------|-------|-------|---------|
| **v2.0.0** | yes | yes | yes |
| anything else | not supported | not supported | not supported |

The extension uses DuckDB 2.0 native query submission, asynchronous tasks, VARIANT
values and the PEG parser token API. Other host versions need their own build.

CI and GitHub Release assets target **v2.0.0** × linux_amd64/arm64, osx_amd64/arm64, windows_amd64.
