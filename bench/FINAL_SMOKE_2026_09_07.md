# Final benchmark smoke — 2026-09-07

This record distinguishes the controlled benchmark evidence from the dated
[benchmark review](REVIEW_2026_09_07.md). It uses an isolated PostgreSQL
database at `127.0.0.1:29389/quackbench`; it does not touch the shared pgEdge
instance. The runner records source and binary hashes, dependency versions,
worker budgets, raw k6 CSV, summaries, server logs, and row-level write checks
in each immutable result directory.

## Harness remediation

- Both w1 and w8 stacks receive 32 aggregate HTTP workers and 32 aggregate
  PostgreSQL pool slots. The w8 configuration is eight processes with four
  HTTP workers and four pool slots per process.
- The runner waits for all requested worker PIDs, refuses occupied ports, and
  only stops the process group it created.
- Report rows correlate raw k6 samples through the runner's `export_name`
  manifest rather than parsing routing fields from a filename. A cell is valid
  only when the measurement request count, successful count, raw latency count,
  HTTP/check results, and (for writes) committed/acknowledged counts agree.
- Write warmup drains before measurement begins. The write workload emits an
  explicit measurement-success counter and the row check excludes warmup rows.

## Focused write regression

The post-remediation eight-worker QuackAPI write run is preserved at
`bench/results/20260907T083838Z-39076`.

| Stack | VUs | Measurement requests | Successful writes | Committed rows | p50 | p99 | Result |
|---|---:|---:|---:|---:|---:|---:|---|
| quackapi-w8 | 8 | 58,237 | 58,237 | 58,237 | 0.158 ms | 1.246 ms | valid |

The measurement window was three seconds after a one-second warmup and a
two-second warmup drain. Four requests had a roughly ten-second tail. This
matches QuackAPI's current httplib transport: a worker owns an accepted socket
until its ten-second keep-alive timeout, while each w8 process has four worker
threads. Four idle reused connections can therefore strand later accepts in one
SO_REUSEPORT process's queue. The delayed requests completed successfully and
are included in the row-level acknowledgement check. The write-only 30-second
k6 graceful stop prevents a server-side commit after client-side cancellation
from being misreported as data loss. The head-of-line blocking is a production
limitation to address with bounded connection lifecycle or nonblocking
keep-alive handling; it is not a throughput claim.

## Next performance priority

Keep the reuse baseline unchanged. The next design iteration should move idle
keep-alive sockets off request-worker threads, with bounded per-connection
state and explicit admission when the active request queue is full. Validate it
with both the existing closed-loop VU matrix and fixed arrival-rate workloads
that report offered versus achieved rate, queue delay, timeouts, and per-worker
connection distribution. That work is necessary before treating the current
reuse-path numbers as production performance parity.

## Final comparison status

The final all-stack smoke is pending the consolidated validation build. It will
run QuackAPI and FastAPI at one and eight workers across `hello`, `item`,
`rows`, and `write`, with item and write at 1 and 8 VUs, using the same isolated
PostgreSQL tables. This document will be updated with the preserved result path
and validity outcome before any comparison is published.
