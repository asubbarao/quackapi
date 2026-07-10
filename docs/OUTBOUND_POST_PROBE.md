# Outbound-POST mechanism probe — http_client vs shellfs+curl

**Date:** 2026-07-10 · **Purpose:** settle the handler/auth-side outbound HTTP
mechanism before building OAuth2 #335 (token exchange = server-side POST).
All numbers from real runs on this machine; setup below is re-runnable.

## Setup

- Target: node one-liner HTTP server on 18473, 1s artificial delay, request
  counter logged (node used ONLY as probe target — it parallelizes properly,
  which the measurement requires; a quackapi/shellfs target was tried first and
  serializes, see finding 3).
- Client under test: quackapi `serve_brain` on 18472, one POST route per
  mechanism, `ab -n 8 -c 8` (8 concurrent handlers on 8 worker connections —
  the same execution context the auth stage uses).

## Findings

### 1. http_client with CONSTANT args sends every request TWICE (disqualifying trap)

One `SELECT http_post('http://…', …)` with literal args produced **two** POSTs
at the target (target's request counter; reproduced in a bare CLI with no
quackapi involved). Cause: DuckDB constant-folds the scalar during planning —
executing it — then executes it again at runtime. Latency is 2× and, fatally
for OAuth, a one-time authorization code would be **double-spent** (providers
reject or revoke on code reuse as replay protection).

**Fold-proof shapes (verified single-send):** argument sourced from a table
column, or a prepared-statement bind (`http_post(?::VARCHAR, …)`). The OAuth
implementation MUST use one of these — which it would anyway, since the code
arrives as untrusted request input and our invariant is binds-not-splices.

### 2. Both mechanisms serialize under concurrency inside one process

8 concurrent outbound POSTs (1s target) through 8 worker connections:

| mechanism | single-shot | c8 n8 total | verdict |
|---|---|---|---|
| http_client, constant args | 2.00s (double-send) | 9.0s | broken shape — never use |
| http_client, fold-proof | 1.00s | **7.0s** | serializes (~global lock in ext) |
| shellfs `read_text('curl … \|')` | 1.01s | **7.0s** | serializes (see finding 3) |

Parallel would be ~1–2s. Neither mechanism parallelizes; it's a tie on
concurrency.

### 3. shellfs pipe reads serialize/degrade badly under concurrent load (independent finding)

A quackapi route whose handler is just `read_text('date +%s%N |')` (~15ms
single-shot) took **10.0s for 16 requests at c8** — ~630ms/request under
concurrency. Fresh output per request proves the shell runs each time; the
degradation is contention (shellfs/pipe-open under many threads), not caching.
Curiosity noted en route: `sleep 0.5` inside a shellfs pipe returns in ~0.3s
(fractional sleep truncated somewhere) while `sleep 1`/`sleep 2` behave
linearly — use integer sleeps in shellfs-based probes.

Implication beyond this probe: shellfs in HOT handler paths is a concurrency
hazard; fine for boot hooks, lifecycle, and low-QPS handlers.

## Verdict: http_client via prepared binds / table-sourced args

Concurrency is a tie, single-shot latency is a tie (in the fold-proof shape),
so the decider is **the no-splice invariant**: OAuth's `code`/`state` params
are attacker-influenced request input. shellfs+curl puts them on a shell
command line — a command-injection surface no amount of escaping makes
comfortable. http_client takes them as bound SQL values, same as every other
untrusted value in the stack (docs/SECURITY.md invariant 3).

Serialization is acceptable for the OAuth use case: token exchanges happen at
login frequency, not per-request; concurrent logins queue ~hundreds of ms each.
Documented as a known limit; revisit if a high-QPS outbound use case appears
(candidate fix: dispatch the exchange from a small C-side thread instead of a
worker connection, or upstream a fix to the extension's locking).

**Standing rules for the OAuth build:**
1. `http_post_form` (token endpoints are `application/x-www-form-urlencoded`)
   with URL + params from prepared binds or registry-table columns — never
   literals (double-send) and never shell splices (injection).
2. Load `http_client` via the same lazy-load path the subscription runner uses
   for radio (host instance; replicas don't need it — auth runs on worker
   writer connections).
