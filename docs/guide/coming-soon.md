# Coming / in progress

These features are **not** documented as done. They are actively designed or building. Do not rely on them in production recipes yet.

| Feature | Intent | Status |
|---------|--------|--------|
| **Access logging / request-id batteries** | `X-Request-ID`, structured access log table | Designed / building |
| **Response compression (gzip / zstd)** | Honor `Accept-Encoding` | Designed / building (miniz available; not wired) |
| **WebSocket routes** | Browser RFC6455 duplex | **Blocked** on transport (HTTP library has no Upgrade API). Use [CREATE STREAM (SSE)](stream.md) instead |
| **OIDC / OAuth2 browser SSO** | `CREATE AUTH … OIDC` | Designed — JWT/API_KEY only today |
| **Signed cookie sessions + CSRF** | Browser session cookies | Designed |
| **Middleware BEFORE/AFTER SQL** | Declarative hooks around handlers | Designed |
| **FORMAT / Accept negotiation** | CSV, NDJSON, Arrow, … | JSON / html / text only today |
| **In-process TestClient** | `quackapi_request(…)` without a port | Designed |
| **Static URL prefix** | Mount `static_dir` under `/assets` | Partial — root `static_dir` only |

Authoritative ledger: [FEATURE_STATUS.md](../FEATURE_STATUS.md) sections 2.5 and 4.

When a row moves to “BUILT & MERGED,” this page will shrink and a full guide will land under `docs/guide/`.
