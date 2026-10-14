# Scope boundaries

This page records areas outside the current QuackAPI HTTP API. These are
not advertised as implemented or as scheduled replacements.

| Surface | Current boundary |
|---|---|
| Browser WebSocket upgrade | QuackAPI does not provide an HTTP WebSocket endpoint. `radio` is a separate extension and does not add WebSocket support to QuackAPI. |
| SSE and streaming responses | QuackAPI returns bounded JSON, NDJSON, or CSV responses; it does not provide the removed `CREATE STREAM`/SSE surface. |
| Row filtering and column masking | Route auth protects endpoints but does not rewrite arbitrary SQL or enforce tenant row policies. Put tenant filters in handler SQL or scoped views. |
| Durable job queue | No built-in queue with lease, acknowledgement, visibility-timeout, and retry guarantees remains. A `radio` message bus does not supply those job semantics. |
| GraphQL, generated table APIs, and source import | Define explicit SQL handlers with `CREATE ROUTE`; these former experimental APIs are not included. |
| Arrow Flight | A separate protocol and deployment choice; it is not hosted by QuackAPI or a runtime dependency. |

See the [migration guide](../MIGRATION.md) for removed SQL names and current
alternatives.
