# Supported DuckDB host — quackapi

**Only DuckDB v1.5.5.** Linux, macOS, and Windows (`windows_amd64`).

```sql
INSTALL quackapi FROM community;
LOAD quackapi;
```

| DuckDB | Linux | macOS | Windows |
|--------|-------|-------|---------|
| **v1.5.5** | yes | yes | yes |
| anything else | not supported | not supported | not supported |

Older hosts (1.5.3, 1.5.4, 1.4.x, …) are **out of scope**. Upgrade DuckDB (and quack) to **1.5.5**.

CI and GitHub Release assets are built only for **v1.5.5** × linux_amd64/arm64, osx_amd64/arm64, windows_amd64.
