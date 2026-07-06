"""
FastAPI mirror of the quackapi demo app.

Mirrors every route registered in app.sql exactly:
  GET  /health                      static 200
  GET  /users/{id}                  path param int, 422 on bad type
  GET  /users                       list all users
  POST /users                       create user (JSON body: name str, age int) -> 201
  GET  /users/{id}/posts/{post_id}  nested path params (both int)
  GET  /search?q=&limit=            q required str, limit optional int le=100
  GET  /events                      SSE stream (tick 1..5)
  GET  /openapi.json                built-in FastAPI
  GET  /docs                        built-in FastAPI swagger UI
  GET  /secure                      header param x-api-key required
  GET  /profile                     cookie param session required
  POST /form-submit                 form-urlencoded (name str, age int)
  GET  /old-home                    307 redirect -> /new-home
  POST /login                       static 200 + Set-Cookie
  POST /upload                      multipart file upload

Note on design differences captured here:
- FastAPI auto-redirects /users/ -> /users with 307 (Starlette default)
- FastAPI returns 405 Method Not Allowed with Allow header
- FastAPI 422 loc format: ["body","field"] for JSON body, ["query","p"] for query, ["path","p"] for path
- quackapi 422 loc format: [location, name] where location is "body"/"query"/"path"
- FastAPI aggregates ALL validation errors in one 422; quackapi also does this
- FastAPI OPTIONS returns 405 (no CORS middleware); quackapi returns 405
- FastAPI HEAD on GET returns 200 with empty body; quackapi returns 200 with empty body (C strips it)
"""

from fastapi import FastAPI, Header, Cookie, UploadFile, File, Form, Request, Response, Query
from fastapi.responses import JSONResponse, RedirectResponse, StreamingResponse
from pydantic import BaseModel, Field
from typing import Optional, Annotated
import asyncio

app = FastAPI(title="quackapi", version="0.1.0")

# ── In-memory users store ─────────────────────────────────────────────────────
# Seeded with same rows as app.sql
_users: dict[int, dict] = {
    1: {"id": 1, "name": "alice", "age": 30},
    2: {"id": 2, "name": "bob", "age": 25},
    3: {"id": 3, "name": "carol", "age": 40},
}
_next_id = 100  # matches users_id_seq START 100


# ── Models ────────────────────────────────────────────────────────────────────
class UserCreate(BaseModel):
    name: str
    age: int


# ── Routes ───────────────────────────────────────────────────────────────────

@app.get("/health")
def health():
    return {"status": "ok"}


@app.get("/users/{id}")
def get_user(id: int):
    user = _users.get(id)
    if user is None:
        # quackapi: SQL SELECT returns empty set -> handler emits no body -> null
        # FastAPI: return null explicitly to match quackapi's 200 status
        return JSONResponse(status_code=200, content=None)
    return JSONResponse(status_code=200, content=user)


@app.get("/users")
def list_users():
    return list(_users.values())


class PostPath(BaseModel):
    id: int
    post_id: int


@app.get("/users/{id}/posts/{post_id}")
def get_post(id: int, post_id: int):
    return {"user_id": id, "post_id": post_id}


@app.get("/search")
def search(
    q: str,
    limit: Annotated[Optional[int], Query(le=100)] = None,
):
    results = [
        u for u in _users.values()
        if u["name"].lower().startswith(q.lower())
    ]
    results.sort(key=lambda u: u["id"])
    if limit is not None:
        results = results[:limit]
    return results


@app.post("/users", status_code=201)
def create_user(user: UserCreate):
    global _next_id
    new_id = _next_id
    _next_id += 1
    obj = {"id": new_id, "name": user.name, "age": user.age}
    _users[new_id] = obj
    return obj


@app.get("/events")
async def events():
    """SSE stream — 5 tick events."""
    async def generate():
        for i in range(1, 6):
            yield f"data: tick {i}\n\n"
            await asyncio.sleep(0)
    return StreamingResponse(generate(), media_type="text/event-stream")


@app.get("/secure")
def secure(
    x_api_key: Optional[str] = Header(None, alias="x_api_key"),
    x_api_key_hyphen: Optional[str] = Header(None, alias="x-api-key"),
):
    """
    Accept x_api_key (underscore form, as quackapi param_schema uses) OR
    x-api-key (hyphen form, standard HTTP convention).
    The test corpus sends 'x_api_key' in the header dict.
    """
    key = x_api_key or x_api_key_hyphen
    if key is None:
        from fastapi import HTTPException
        raise HTTPException(
            status_code=422,
            detail=[{
                "type": "missing",
                "loc": ["header", "x_api_key"],
                "msg": "Field required",
                "input": None,
            }]
        )
    return {"ok": True, "key": key}


@app.get("/profile")
def profile(session: str = Cookie(...)):
    return {"user": session}


@app.post("/form-submit")
def form_submit(
    name: str = Form(...),
    age: int = Form(...),
):
    return {"received_name": name, "received_age": age}


@app.get("/old-home")
def old_home():
    return RedirectResponse(url="/new-home", status_code=307)


@app.post("/login")
def login(response: Response):
    response.set_cookie("session", "abc123", path="/", httponly=True)
    return {}


@app.post("/upload")
async def upload(file: UploadFile = File(...)):
    content = await file.read()
    text = content.decode("utf-8", errors="replace")
    return {
        "filename": file.filename,
        "size": len(text),
        "preview": text[:80],
    }
