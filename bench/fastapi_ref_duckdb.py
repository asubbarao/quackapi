#!/usr/bin/env python3
"""FastAPI + DuckDB (python client) reference for quackapi head-to-head bench.
Each dynamic endpoint performs a real query against the same DuckDB file as quack.
Uses one connection per process (as allowed). 
Same validation + semantics. DB is pre-seeded by the bench runner (users table + data only).
"""

import duckdb
from fastapi import FastAPI, Path, Query
from typing import Optional
import json

app = FastAPI(title="fastapi-ref-duckdb")

DB_PATH = "/tmp/qbench_fast.db"
_conn = None


def get_conn():
    global _conn
    if _conn is None:
        # read_only to avoid any write contention; file pre-created
        _conn = duckdb.connect(DB_PATH, read_only=True)
    return _conn


@app.get("/health")
def health():
    return {"status": "ok"}


@app.get("/users")
def list_users():
    # matches quack handler
    rows = get_conn().execute(
        "SELECT id, name, age FROM users u ORDER BY id"
    ).fetchall()
    return [{"id": r[0], "name": r[1], "age": r[2]} for r in rows]


@app.get("/users/{id}")
def get_user(id: int = Path(...)):
    rows = get_conn().execute(
        "SELECT id, name, age FROM users u WHERE u.id = ?",
        [id],
    ).fetchall()
    if rows:
        r = rows[0]
        return {"id": r[0], "name": r[1], "age": r[2]}
    from fastapi import HTTPException
    raise HTTPException(status_code=404, detail="Not Found")


@app.get("/search")
def search(q: str = Query(...), limit: Optional[int] = Query(None, le=100)):
    if limit is None:
        limit = 100
    rows = get_conn().execute(
        """
        SELECT id, name, age FROM users
        WHERE starts_with(lower(name), lower(?))
        ORDER BY id
        LIMIT ?
        """,
        [q, limit],
    ).fetchall()
    return [{"id": r[0], "name": r[1], "age": r[2]} for r in rows]
