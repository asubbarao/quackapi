# LLM gateway: the outbound-HTTP latency cliff

Research note for the quackapi vs FastAPI bench. Question: why did the LLM-gateway
route (`bench/llm_routes.sql`) reportedly show ~1235ms p50 against FastAPI's ~2.2ms —
a gap three orders of magnitude wide, on a workload quackapi should win?

**Short answer:** it was never an HTTP-client problem, and never a quackapi core
problem. The server holds **one worker thread per in-flight request**, and the
outbound upstream call is **synchronous**. With the bench's `worker_threads=32`,
concurrency above 32 does not queue gracefully — throughput *collapses* and the
latency tail runs to **28 seconds**. Raising `worker_threads` to exceed peak
concurrency removes the cliff entirely and puts quackapi at **~0ms overhead over
the upstream's own service time**.

**The one-line fix for any route that calls an upstream:**

```sql
-- worker_threads must EXCEED peak inbound concurrency for blocking-I/O routes.
SELECT * FROM quackapi_serve(8000, worker_threads := 128 /* not the default 32 */);
```

---

## 1. Two theories that the measurements killed

Both were plausible on paper. Neither survived contact with a benchmark.

**Theory A — "the community `http_client` extension dials a fresh connection every
call."** True as written: `query-farm/httpclient`'s `SetupHttpClient()` constructs a
new `duckdb_httplib_openssl::Client` per call, with no pooling and no setting to
enable it. But both LLM routes target `127.0.0.1:11434` — **loopback, plaintext, no
TLS**. A loopback handshake costs microseconds; there is no round-trip to amortize.
Measured directly, 3000 sequential POSTs:

| function | 3000 sequential POSTs |
|---|---|
| `http_post()` (fresh dial each call) | 0.349s |
| `quackapi_post()` (pooled, 49/50 reused) | 0.363s |

No win. Connection pooling is real and worth having (`f3478d5`), but it pays off
against *remote TLS* upstreams, not loopback. It cannot explain 1235ms.

**Theory B — "it's just Ollama's generation time, both stacks pay it."** Ruled out by
arithmetic: if both stacks paid the same upstream cost, both would report ~1200ms.
A 500x split means the two sides were not doing the same work.

## 2. What is actually happening

Fixed-latency upstream (**100ms**, Python `ThreadingHTTPServer`), a minimal route
calling it, k6 sweeping concurrency. Server: `worker_threads=32` (the bench default).

| VUs | median | avg | max | throughput |
|----:|-------:|----:|----:|-----------:|
| 1 | 105.83ms | 104.97ms | 107.87ms | 9.5/s |
| 8 | 104.75ms | 104.18ms | 106.87ms | 76.5/s |
| 32 | 104.12ms | 103.77ms | 106.69ms | **303.3/s** |
| 64 | 104.25ms | 320.28ms | **17.96s** | 137.1/s |
| 128 | 103.59ms | 675.23ms | **28.18s** | 67.7/s |

The break is exactly at the worker count. Past it, throughput does not plateau at the
theoretical ceiling (~320/s = 32 workers ÷ 0.1s) — it **falls**, 303 → 137 → 68/s,
while the tail explodes to 28 seconds. Note the median stays flat at ~104ms
throughout: most requests are served promptly and a starved minority waits behind
blocked workers. **A p50 alone hides this entirely** — which is why the original
figure was so hard to attribute.

### The upstream is not the bottleneck

The obvious confound is that the Python mock upstream is itself dying under load.
It is not. Hit directly, with quackapi out of the path, it scales linearly and stays
flat:

| VUs | median | throughput |
|----:|-------:|-----------:|
| 32 | 103.24ms | 308.2/s |
| 64 | 103.17ms | 616.0/s |
| 128 | 103.11ms | **1232.2/s** |

Side by side at identical load, the gap is entirely quackapi's:

| VUs | upstream alone | through quackapi |
|----:|---------------:|-----------------:|
| 64 | 616.0/s | 137.1/s (**4.5x worse**) |
| 128 | 1232.2/s | 67.7/s (**18x worse**) |

## 3. The fix, measured

Same everything, `worker_threads` raised 32 → 128. A no-upstream route (`SELECT 1`)
is included to separate server overhead from the outbound path.

| route | VUs | before (32 workers) | after (128 workers) |
|---|----:|---|---|
| `/probe` (outbound HTTP) | 64 | 137.1/s, max **17.96s** | **603.1/s, max 107.56ms** |
| `/probe` (outbound HTTP) | 128 | 67.7/s, max **28.18s** | **752.4/s, max 106.93ms** |
| `/hello` (no HTTP) | 64 | — | **85,234/s**, med 478µs |
| `/hello` (no HTTP) | 128 | — | **82,732/s**, med 888µs |

Two conclusions:

1. **The server core was never the problem.** 82k rps and sub-millisecond medians on
   a route that does no I/O.
2. **At adequate worker count there is no measurable framework tax.** quackapi
   answers in **103.57ms** against an upstream that itself takes **103.11ms**. The
   multi-second tail is gone; max latency is 106.93ms at 128 concurrent.

## 4. Why this looked like 1235ms with Ollama

Substitute a real generation call (~1200ms) for the 100ms mock. Every in-flight
request now pins a worker for 1.2 seconds, so a 32-worker pool saturates at trivial
concurrency, and each additional round of queueing adds another full 1.2s. FastAPI's
`await client.post(...)` yields the event loop and never blocks, so it shows no such
cliff. The comparison was measuring **thread-pool starvation**, not framework speed.

Two caveats, stated plainly:

- The provenance of the original `1235ms vs 2.2ms` pair could not be found in any
  committed file (`bench/`, `docs/`, `README.md`). It is an uncaptured number from an
  earlier session. A FastAPI p50 of 2.2ms is far too fast for a real `generate` call,
  so that side was likely not doing the same work (or erroring fast) — the two
  numbers may never have been a fair pair. **This note deliberately does not rely on
  them**; the mechanism above is measured from scratch.
- These runs use a fixed-latency mock, not live Ollama (no model is currently pulled
  on this machine). That is a feature for isolating *framework* overhead, but the
  end-to-end Ollama numbers still want a rerun once a model is available.

## 5. Likely explains the open `PG_ATTACH_CONCURRENCY` finding too

`PG_ATTACH_CONCURRENCY.md` records a throughput collapse at **32 VUs** on the
Postgres `ATTACH` path, notes that raising `pg_pool_max_connections` does not fix it,
and leaves the root cause unresolved as "out of scope." `serve_quackapi.sh` boots
that stack with `worker_threads=32`. The collapse threshold, the shape of the curve,
and the immunity to pool tuning all match what is measured here. Worth re-running
that sweep with `worker_threads=128` before spending more effort on the PG pool.

## 6. Recommendations

1. **Size `worker_threads` to peak concurrency, not to core count.** The default
   `QUACKAPI_DEFAULT_WORKER_THREADS = 32` is a reasonable figure for CPU-bound routes
   and a foot-gun for routes that block on an upstream. Consider documenting it as
   such, or scaling it up when a route is known to make outbound calls.
2. **Record max/p99, not just p50, for any route with an upstream.** The median was
   flat and healthy at 104ms in the exact runs where the tail hit 28 seconds.
3. **Keep the `http_client` → `quackapi_post()` swap** in `bench/llm_routes.sql` —
   not for the pooling (which is a no-op on loopback), but because it drops a
   third-party dependency its own README labels "very experimental," and exposes
   `.reused_connection` for diagnostics.
4. **The real architectural note:** one blocked thread per in-flight upstream call
   means 1000 concurrent requests wants 1000 threads, where an async client needs
   one. Config closes the gap at bench scale; it does not make the model async.
