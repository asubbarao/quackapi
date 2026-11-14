# SQL execution logging

Enable DuckDB's Metrics log type to collect SQL execution history:

```sql
CALL enable_logging(['QueryLog','Metrics','HTTP','Quack'], storage := 'stdout');
```

QuackAPI attaches a small ClientContextState to existing and future connections.
It emits statement.total_time, statement.sql, statement.success, and an optional
statement.error when a begun statement finishes. Timing uses a monotonic clock;
UTC start/end and native connection/query IDs remain attached to the event.
This does not enable the operator profiler or change caller profiler settings.
Statements carry their submission ID and immutable parent connection/query IDs.
Parent metadata is captured before scheduling, so it does not retain a caller.
A completed or failed submission clears its context before connection reuse.

HTTP SQL batches and native self-dispatch also emit request.total_time,
request.sql, request.success, request.origin, and an optional request.error.
These are submission measurements, separate from individual statement timing.
They preserve SQL rejected during parsing; native scalar and resumable paths
both identify their origin as selfdispatch. HTTP execution uses quackapi.

Metrics values use DuckDB's native log serialization. Success is explicit because
a NULL Value serializes to the string NULL. Never infer success from that string.

A direct local Connection::Query parser rejection occurs before DuckDB's
statement lifecycle callbacks. Queued rows abandoned before execution also do
not produce a completed submission event. These are coverage boundaries, not
successful or zero-duration executions. SQL source files and agent identity
require explicit caller metadata; connection IDs are not inferred identities.

Build and run the candidate regression with make query_metrics_release after
building quackapi_metrics_test and quackapi_loadable_extension. It loads the
explicit candidate path and exercises existing/new connections, nested native
dispatch, binding failures, parser rejection, and recorded submission origin.
