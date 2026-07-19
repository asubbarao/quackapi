# FastAPI-equivalence conformance suite

Live HTTP checks against `quackapi_serve()` asserting FastAPI-documented behavior
([fastapi.tiangolo.com](https://fastapi.tiangolo.com/)).

## Run

```bash
# from repo root, after make release
bash test/conformance/run.sh
python3 test/conformance/render_scorecard.py
```

Override port/binary:

```bash
PORT=18888 DUCKDB=./build/release/duckdb bash test/conformance/run.sh
```

## Layout

| Path | Role |
|------|------|
| `routes.sql` | Fixture routes (CREATE ROUTE / CREATE AUTH) |
| `cases.jsonl` | Behavior corpus |
| `driver.py` | Fires requests; writes `results/results.jsonl` |
| `run.sh` | FIFO interactive serve → drive → stop |
| `render_scorecard.py` | Headline PASS/FAIL/N/A + classes |

FIFO (not `duckdb -c`) is required so parser-extension DDL after LOAD and a live serve work.
