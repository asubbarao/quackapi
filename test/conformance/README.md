# FastAPI-equivalence HTTP contract suite

Live differential HTTP checks between a running `quackapi_serve()` instance
and a live, pinned FastAPI reference app (`fastapi_app.py`, started by
`driver.py` via uvicorn). Every case in `cases.jsonl` is fired at **both**
implementations and their status, headers, and body are compared
structurally. There is no hand-encoded "what FastAPI would say" left
anywhere in this suite — the reference is a real, running program, and if
it isn't reachable the run fails loudly rather than reporting equivalence
without evidence.

A `FAIL` always means the two live responses actually disagreed (or
quackapi violated a named, pinned assertion). Some disagreements are real
and understood — a Starlette routing quirk, or a case where quackapi does
*more* than plain FastAPI — and those get an explicit `class_hint` in
`cases.jsonl` (`FASTAPI-QUIRK`, `STRONGER`, ...) for readability. That
classification is commentary only: it is sourced exclusively from a
human-reviewed `class_hint` field on the case, never inferred at runtime
from notes text or the case id, and it never changes the verdict or the
process exit code. A `FAIL` reads as a failure.

## Run

```bash
# from repo root, after make release
bash test/conformance/run.sh
python3 test/conformance/render_scorecard.py
```

`driver.py` provisions its own FastAPI reference: if the interpreter
running it doesn't already have `fastapi`/`uvicorn` importable, it creates
a one-time venv at `test/conformance/.venv` and installs
`requirements.txt` into it (see `ensure_reference_python` in `driver.py`).
This is deliberately plain Python, not a shell script — see the
[no-shell note](#no-shell-note) below.

Override port/binary/reference:

```bash
PORT=18888 DUCKDB=./build/release/duckdb bash test/conformance/run.sh
# or run the driver directly against already-running servers:
python3 test/conformance/driver.py --base http://127.0.0.1:18770 \
  --fastapi-base http://127.0.0.1:18771 --no-start-fastapi
```

## Failure modes

- **FastAPI reference unreachable at start** (dead port, process failed to
  boot): the driver exits non-zero before writing any results file. A
  missing reference is a run failure, never equivalence.
- **FastAPI reference stops responding mid-run**: the driver aborts
  immediately (non-zero exit), still without writing results. A partial
  run must never be read as a completed one.
- **Stale/foreign `results.jsonl`**: `render_scorecard.py` recomputes the
  sha256 of the `cases.jsonl` on disk and compares it to the one embedded
  in `results/summary.json`, checks the case id set and count match
  exactly, checks `reference_reachable: true`, and refuses anything older
  than `--max-age-seconds` (default 24h). Any mismatch is refused with the
  specific check named — never silently rendered.

## Layout

| Path | Role |
|------|------|
| `routes.sql` | Fixture routes (CREATE ROUTE / CREATE AUTH) |
| `cases.jsonl` | Behavior corpus |
| `fastapi_app.py` | Pinned live FastAPI reference, mirroring `routes.sql` route-for-route |
| `requirements.txt` | Pins for `fastapi_app.py`'s venv |
| `driver.py` | Starts the reference, fires every case at both implementations, compares, writes `results/results.jsonl` + `results/summary.json` |
| `run.sh` | FIFO interactive serve → drive → stop (legacy shell — see below) |
| `render_scorecard.py` | Verifies a results file is current and reference-backed, then renders PASS/FAIL/N/A + classes |
| `test_driver.py` | Unit tests for the comparator (`evaluate`, `classify`, `types_equivalent`, `parse_set_cookie`) |

FIFO (not `duckdb -c`) is required so parser-extension DDL after LOAD and a
live serve work.

## `fastapi_app.py` design note

Each handler mirrors the *actual SQL* in the matching `routes.sql` route
(e.g. `GET /users/{id}` returns `[u for u in USERS if u["id"]==id]` — a
list, because that's what `SELECT ... WHERE id = $id` is) rather than an
idiomatic single-object REST shape. This makes most bodies directly,
structurally comparable instead of needing per-case envelope adapters, and
it's what let several previously-assumed "intentional envelope
difference" cases collapse into genuine matches once actually run.

quackapi also has a real, documented magic-column convention that isn't
obvious from the SQL alone: a `SELECT` whose single column is literally
named `text` or `html` renders as a raw `text/plain`/`text/html` body
instead of the usual JSON array-of-rows (confirmed live: `GET
/status/created` returns the bare string `created`, not
`[{"text":"created"}]`). `status_created`, `status_teapot`, and
`status_nocontent` in `routes.sql` all use `AS text`, so the reference
mirrors that.

Two divergences are deliberately **not** papered over:

- `/users/{id}` has no `@app.head` decorator. quackapi answers `HEAD` on
  any `GET` route automatically, with no explicit registration
  (`get_user_head_explicit` in `cases.jsonl`, `class_hint: STRONGER`); bare
  Starlette 405s there without one. Adding a matching decorator would hide
  a real strength behind a fabricated match.
- Same idea for `get_user_overflow`: FastAPI/Pydantic happily parse an
  arbitrary-precision Python int and return `200 []`; quackapi enforces an
  int64 bound and 422s. `class_hint: STRONGER`.

One genuine FastAPI/Starlette limitation surfaces as a real, permanent
mismatch rather than something to route around: `Router.app`'s 405 handler
(`starlette.routing`) only keeps the *first* partial-method-match route
for a path when building the `Allow` header, so two separate
single-method decorators on the same path (`@app.get` + `@app.head`) never
get merged into one `Allow` value the way quackapi's registry does. This
shows up on the `health_*_405`/`method_mismatch_*` cases,
`class_hint: FASTAPI-QUIRK`.

## Python 3.14 pin note

`requirements.txt`'s versions (`fastapi==0.141.1 uvicorn==0.53.0
starlette==1.6.0 pydantic==2.13.5 python-multipart==0.0.32`) are the
*newest* available, not an arbitrary floor. This machine's only Python is
3.14, and older common pins (e.g. `fastapi==0.115.x` / `pydantic==2.10.x`)
have no prebuilt `pydantic-core` wheel for 3.14 — `pip install` tries to
build it from source via maturin/PyO3, which rejects 3.14 outright
("the configured Python interpreter version (3.14) is newer than PyO3's
maximum supported version"). If the target Python changes, re-pin
deliberately; don't just relax the pins to "whatever installs."

## No-shell note

This repo is mid-migration off shell scripts: `bench/no-shell` (commit
`655bed7`, not yet merged into every branch) deleted every `.sh` file and
rebuilt those pipelines as `.sql` + shellfs. `test/conformance/run.sh`
here predates that migration and is left as-is — converting it is a
separate concern from this suite's equivalence work, and doing it
piecemeal would duplicate `bench/no-shell`'s own eventual pass over this
directory. No new `.sh` content was added for this work: the one thing
`run.sh` used to not know how to do (get a Python with `fastapi`/`uvicorn`
onto the path) is handled entirely inside `driver.py`
(`ensure_reference_python`), in Python, so `run.sh` didn't need to grow.
