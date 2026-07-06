"""
quackapi conformance reference — FastAPI mirror of app.sql + framework.sql routes.

Mirrors every route registered in app.sql exactly:
  GET  /health                      static 200 {"status":"ok"}
  GET  /users/{id}                  path param int, 422 on bad type
  GET  /users                       list all users
  POST /users                       JSON body {name:str, age:int} -> 201
  GET  /users/{id}/posts/{post_id}  nested path params (both int)
  GET  /search?q=&limit=            q required str, limit optional int le=100
  GET  /events                      SSE stream (tick 1..5)
  GET  /openapi.json                built-in FastAPI
  GET  /docs                        built-in FastAPI Swagger UI
  GET  /secure                      header param x-api-key required
  GET  /profile                     cookie param session required
  POST /form-submit                 form-urlencoded (name str, age int)
  GET  /old-home                    307 redirect -> /new-home
  POST /login                       static 200 + Set-Cookie
  POST /upload                      multipart file upload

Usage:
  uv run --with fastapi==0.115.12 --with uvicorn==0.34.2 --with 'python-multipart==0.0.20' \
    python reference_app.py --port 18650

Design-difference notes (intentional divergences from FastAPI defaults captured here):
- GET /users/{id} missing user: FastAPI returns 200 null (matching quackapi empty-result behavior)
  quackapi: SQL returns no rows -> 404 from C server or empty result from oracle
- FastAPI auto-redirects trailing slashes (/users/ -> 307); quackapi: 404
- FastAPI 422 detail items include an "input" field; quackapi does not
- FastAPI header param loc uses lowercased header name with dash (x-api-key);
  quackapi uses underscore form (x_api_key) from param_schema name
- FastAPI coerces age="5" (str) -> int 5 (Pydantic v2 strict=False default);
  quackapi treats "5" as string -> int_parsing 422
- FastAPI coerces age=true -> 1, age=false -> 0; quackapi: NOT pydantic, uses try_cast
  which treats true/false as valid booleans but NOT valid integers -> 422 (or 201 in oracle)
- FastAPI body model with wrong Content-Type (text/plain) returns 422;
  quackapi still parses JSON regardless of Content-Type
- FastAPI /openapi.json includes URL/path parameters in correct OpenAPI structure;
  quackapi generates its own schema format (structurally different but semantically similar)
"""

from __future__ import annotations

import argparse
import asyncio
from typing import Annotated, Optional

import uvicorn
from fastapi import Cookie, FastAPI, File, Form, Header, Query, Response, UploadFile
from fastapi.responses import JSONResponse, RedirectResponse, StreamingResponse

app = FastAPI(title="quackapi", version="0.1.0")

# ── In-memory users store (mirrors app.sql seed data) ──────────────────────────
# Seeded with same 3 rows as app.sql:
#   INSERT INTO users (id, name, age) SELECT 1,'alice',30 UNION ALL ...
_users: dict[int, dict] = {
    1: {"id": 1, "name": "alice", "age": 30},
    2: {"id": 2, "name": "bob", "age": 25},
    3: {"id": 3, "name": "carol", "age": 40},
}
# Matches CREATE SEQUENCE users_id_seq START 100
_next_id = 100


# ── Models ────────────────────────────────────────────────────────────────────
from pydantic import BaseModel


class UserCreate(BaseModel):
    name: str
    age: int


# ── Routes ───────────────────────────────────────────────────────────────────

@app.get("/health")
def health() -> dict:
    return {"status": "ok"}


@app.get("/users/{id}")
def get_user(id: int) -> JSONResponse:
    user = _users.get(id)
    if user is None:
        # quackapi SQL returns no rows for missing user; C server returns 404.
        # FastAPI has no implicit 404 here. We return 200 null to mirror the SQL oracle
        # behavior (SELECT returns nothing -> empty result).
        # DIVERGE NOTE: C server actually returns 404 for empty handler result;
        # SQL oracle returns 200 with null body. Reference here matches SQL oracle.
        return JSONResponse(status_code=200, content=None)
    return JSONResponse(content=user)


@app.get("/users")
def list_users() -> list:
    return list(_users.values())


@app.get("/users/{id}/posts/{post_id}")
def get_post(id: int, post_id: int) -> dict:
    return {"user_id": id, "post_id": post_id}


@app.get("/search")
def search(
    q: str,
    limit: Annotated[Optional[int], Query(le=100)] = None,
) -> list:
    results = [u for u in _users.values() if u["name"].lower().startswith(q.lower())]
    results.sort(key=lambda u: u["id"])
    if limit is not None:
        results = results[:limit]
    return results


@app.post("/users", status_code=201)
def create_user(user: UserCreate) -> dict:
    global _next_id
    new_id = _next_id
    _next_id += 1
    obj = {"id": new_id, "name": user.name, "age": user.age}
    _users[new_id] = obj
    return obj


@app.get("/events")
async def events() -> StreamingResponse:
    """SSE stream: 5 tick events, matching 'SELECT tick || i FROM range(1,6)'."""
    async def generate():
        for i in range(1, 6):
            yield f"data: tick {i}\n\n"
            await asyncio.sleep(0)

    return StreamingResponse(generate(), media_type="text/event-stream")


@app.get("/secure")
def secure(x_api_key: str = Header(...)) -> dict:
    return {"ok": True, "key": x_api_key}


@app.get("/profile")
def profile(session: str = Cookie(...)) -> dict:
    return {"user": session}


@app.post("/form-submit")
def form_submit(
    name: str = Form(...),
    age: int = Form(...),
) -> dict:
    return {"received_name": name, "received_age": age}


@app.get("/old-home")
def old_home() -> RedirectResponse:
    return RedirectResponse(url="/new-home", status_code=307)


@app.post("/login")
def login(response: Response) -> dict:
    response.set_cookie("session", "abc123", path="/", httponly=True)
    return {}


@app.post("/upload")
async def upload(file: UploadFile = File(...)) -> dict:
    content = await file.read()
    text = content.decode("utf-8", errors="replace")
    return {
        "filename": file.filename,
        "size": len(text),
        "preview": text[:80],
    }


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--port", type=int, default=18650)
    parser.add_argument("--host", default="127.0.0.1")
    args = parser.parse_args()
    uvicorn.run(app, host=args.host, port=args.port, log_level="warning")
