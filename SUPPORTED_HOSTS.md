# Supported DuckDB hosts — quackapi

## Two install paths (do not mix them up)

### 1) `INSTALL quackapi FROM community` (what most people use)

DuckDB downloads from:

`https://community-extensions.duckdb.org/v{EXACT_DUCKDB_VERSION}/{platform}/quackapi.duckdb_extension.gz`

| DuckDB host | Linux | macOS | Windows |
|-------------|-------|-------|---------|
| **v1.5.5** (community tip) | yes | yes | **yes** |
| **v1.5.4** | yes | yes | **no** (CDN 404) |
| **v1.5.3** (old quack) | **no** | **no** | **no** |

Community only **rebuilds for the current stable** when a pin PR merges. It does **not** fill old folders like `v1.5.3/`.

**Windows users:** use **DuckDB v1.5.5** + `INSTALL quackapi FROM community`.

### 2) GitHub Releases (our multi-host matrix)

Built by CI for **v1.5.3, v1.5.4, v1.5.5** × linux/macOS/windows.  
Use when you are stuck on **1.5.3** (e.g. quack) or need Windows on a host community never published.

```sql
-- after downloading the matching asset:
LOAD '/path/to/quackapi-duckdb-v1.5.3-windows_amd64.duckdb_extension';  -- needs duckdb -unsigned
```

## What we recommend

| You run | Do this |
|---------|---------|
| New installs / Windows | **DuckDB 1.5.5** + community |
| Quack still on 1.5.3 | **Bump quack to 1.5.5** (best) *or* load a Release asset for v1.5.3 |
| Want latest gemini-surface features | Community pin must be updated (PR) *or* build from `main` |

## Bottom line

- **Windows + community = yes on 1.5.5 already.**
- **1.5.3 + community = never**, for any OS, unless community changes policy.
- **1.5.3 + all OS** only via **our Release builds**, not `FROM community`.
