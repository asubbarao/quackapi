"""FastAPI benchmark stack B — same four routes as quackapi, pgEdge Postgres as data plane."""

from __future__ import annotations

from datetime import datetime
from decimal import Decimal
import os
from typing import Any

import anyio
from fastapi import FastAPI, Query
from psycopg.rows import dict_row
from psycopg_pool import ConnectionPool
from pydantic import BaseModel
from bench_config import pg_dsn

# Match the effective per-process budget emitted by serve_fastapi.sh:
# 32 worker/pool slots at w1 and 8/4 at w8, for an aggregate 32/32 budget.
THREADPOOL_SIZE = int(os.environ.get("BENCH_WORKER_THREADS", "32"))
PG_POOL_MAX = int(os.environ.get("BENCH_PG_POOL_MAX", str(THREADPOOL_SIZE)))

# pgEdge Postgres 17 (spock multi-master) — every request hits this live DB.
PG_DSN = pg_dsn()

app = FastAPI()
pool: ConnectionPool | None = None


class WriteBody(BaseModel):
    id: int
    note: str


def _serialize_value(v: Any) -> Any:
    if isinstance(v, datetime):
        return v.isoformat(sep="T")
    if isinstance(v, Decimal):
        return float(v)
    return v


def _row_dicts(cur) -> list[dict[str, Any]]:
    rows = cur.fetchall()
    return [{k: _serialize_value(v) for k, v in row.items()} for row in rows]


@app.on_event("startup")
def on_startup() -> None:
    global pool
    # Starlette runs plain `def` handlers in AnyIO's default threadpool.
    # Raise the limit to match the effective per-process QuackAPI worker pool.
    anyio.to_thread.current_default_thread_limiter().total_tokens = THREADPOOL_SIZE
    # Pool sized for the same concurrency ceiling as the threadpool.
    pool = ConnectionPool(
        conninfo=PG_DSN,
        min_size=2,
        max_size=PG_POOL_MAX,
        kwargs={"row_factory": dict_row, "autocommit": True},
        open=True,
    )


@app.on_event("shutdown")
def on_shutdown() -> None:
    global pool
    if pool is not None:
        pool.close()
        pool = None


@app.get("/hello")
def hello() -> list[dict[str, str]]:
    return [{"msg": "world"}]


@app.get("/items/{id}")
def get_item(id: int) -> list[dict[str, Any]]:
    # Path param typed as int → FastAPI returns native 422 {"detail":[...]} on bad values.
    assert pool is not None
    with pool.connection() as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name FROM bench_rows WHERE id = %s",
                (id,),
            )
            return _row_dicts(cur)


@app.get("/rows")
def get_rows(n: int = Query(...)) -> list[dict[str, Any]]:
    assert pool is not None
    with pool.connection() as conn:
        with conn.cursor() as cur:
            cur.execute(
                "SELECT id, name, value, ts FROM bench_rows ORDER BY id LIMIT %s",
                (n,),
            )
            return _row_dicts(cur)


@app.post("/write")
def write_row(body: WriteBody) -> list[dict[str, int]]:
    assert pool is not None
    with pool.connection() as conn:
        with conn.cursor() as cur:
            cur.execute(
                "INSERT INTO bench_writes (id, note) VALUES (%s, %s) RETURNING id",
                (body.id, body.note),
            )
            return _row_dicts(cur)
