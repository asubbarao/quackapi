#!/usr/bin/env python3
"""FastAPI in-memory reference for quackapi head-to-head bench.
Endpoints return semantically identical data to the quackapi app.sql handlers.
Uses real FastAPI param validation (Path int, Query(le=100)).
Responses are standard dicts (will have JSON whitespace diffs vs quack compact; noted in report).
"""

from fastapi import FastAPI, Path, Query
from typing import Optional

app = FastAPI(title="fastapi-ref-mem")

# Same seed data as app.sql + framework.sql
USERS = [
    {"id": 1, "name": "alice", "age": 30},
    {"id": 2, "name": "bob", "age": 25},
    {"id": 3, "name": "carol", "age": 40},
]


@app.get("/health")
def health():
    # exact static body quack serves
    return {"status": "ok"}


@app.get("/users")
def list_users():
    # SELECT coalesce(json_group_array(to_json(u)), '[]') FROM users u
    return USERS


@app.get("/users/{id}")
def get_user(id: int = Path(...)):
    # SELECT to_json(u) FROM users u WHERE u.id = {id}
    for u in USERS:
        if u["id"] == id:
            return u
    # not hit in bench matrix; quack handler on miss would produce null body
    from fastapi import HTTPException
    raise HTTPException(status_code=404, detail="Not Found")


@app.get("/search")
def search(q: str = Query(...), limit: Optional[int] = Query(None, le=100)):
    # SELECT ... WHERE starts_with(lower(name), lower({q})) ORDER BY id LIMIT coalesce({limit},100)
    if limit is None:
        limit = 100
    prefix = q.lower()
    res = []
    for u in USERS:
        if u["name"].lower().startswith(prefix):
            res.append(u)
            if len(res) >= limit:
                break
    return res
