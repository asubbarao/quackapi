"""Pinned live reference implementation, mirroring test/conformance/routes.sql route for route.

driver.py starts this app with uvicorn and fires every case at it exactly as it fires
the same case at quackapi. It is the equivalence oracle: there is no hand-encoded
"what FastAPI would say" anywhere else in this harness. When this app's answer changes,
conformance's answer changes with it.

Pinned versions (see requirements.txt): fastapi==0.141.1 uvicorn==0.53.0
starlette==1.6.0 pydantic==2.13.5 python-multipart==0.0.32.
"""

from __future__ import annotations

import base64
import hashlib
import hmac
import json
from typing import Any

from fastapi import (
    Cookie,
    Depends,
    FastAPI,
    Header,
    HTTPException,
    Query,
    Request,
    Response,
)
from fastapi.responses import HTMLResponse, PlainTextResponse, RedirectResponse
from pydantic import TypeAdapter, ValidationError

FASTAPI_PINNED_VERSION = "0.141.1"

app = FastAPI(title="quackapi-conformance-reference")

USERS = [
    {"id": 1, "name": "alice", "age": 30},
    {"id": 2, "name": "bob", "age": 25},
    {"id": 3, "name": "carol", "age": 40},
]

API_KEYS = {"k-secret": "alice"}
JWT_SECRET = b"conformance-secret"

_INT_ADAPTER: TypeAdapter[int] = TypeAdapter(int)


def _pydantic_field_error(
    adapter: TypeAdapter[Any], raw: Any, loc: list[str]
) -> dict | None:
    try:
        adapter.validate_python(raw)
        return None
    except ValidationError as exc:
        err = exc.errors()[0]
        return {"loc": loc, "msg": err["msg"], "type": err["type"]}


@app.get("/health")
def health() -> list[dict]:
    return [{"status": "ok"}]


# Starlette does not auto-add HEAD to a GET route; routes.sql registers HEAD
# for /health explicitly (CREATE ROUTE health_head HEAD '/health'), so the
# reference must too. /users/{id} deliberately has NO such decorator below:
# quackapi answers HEAD there anyway, with no explicit registration, which
# is a real divergence (quackapi does more than bare FastAPI) that
# get_user_head_explicit in cases.jsonl is meant to surface, not hide.
@app.head("/health")
def health_head() -> Response:
    body_len = len(json.dumps(health()).encode("utf-8"))
    return Response(
        status_code=200,
        media_type="application/json",
        headers={"content-length": str(body_len)},
    )


@app.get("/users/{id}")
def get_user(id: int) -> list[dict]:
    return [u for u in USERS if u["id"] == id]


@app.get("/users")
def list_users() -> list[dict]:
    return sorted(USERS, key=lambda u: u["id"])


@app.get("/users/{id}/posts/{post_id}")
def get_post(id: int, post_id: int) -> list[dict]:
    return [{"user_id": id, "post_id": post_id}]


@app.get("/search")
def search(q: str) -> list[dict]:
    needle = q.lower()
    return [
        u
        for u in sorted(USERS, key=lambda u: u["id"])
        if u["name"].lower().startswith(needle)
    ]


@app.get("/search_limit")
def search_limit(q: str, limit: int = Query(10, le=100)) -> list[dict]:
    needle = q.lower()
    rows = [
        u
        for u in sorted(USERS, key=lambda u: u["id"])
        if u["name"].lower().startswith(needle)
    ]
    return rows[:limit]


@app.get("/echo")
def echo(q: str) -> list[dict]:
    return [{"q": q}]


@app.get("/flag")
def flag(on: bool) -> list[dict]:
    return [{"on": on}]


@app.get("/price")
def price(amount: float) -> list[dict]:
    return [{"amount": amount}]


def _resolve_create_user(
    name_q: str | None, age_q: str | None, raw_body: bytes, content_type: str
) -> tuple[str | None, int | None, list[dict]]:
    """Query params win when present; JSON body fills whatever query left empty.

    This mirrors quackapi's observed binder precedence for `CREATE ROUTE create_user
    POST '/users' AS SELECT $name, $age ...` (no PARAM clause pins a single source),
    captured by running the live extension against routes.sql before writing this.
    """
    body_obj: dict = {}
    body_level_error: dict | None = None
    if raw_body:
        if "application/json" in content_type:
            try:
                parsed = json.loads(raw_body)
                if isinstance(parsed, dict):
                    body_obj = parsed
                else:
                    body_level_error = {
                        "loc": ["body"],
                        "msg": "Input should be a valid dictionary or object to extract fields from",
                        "type": "model_attributes_type",
                    }
            except json.JSONDecodeError:
                body_level_error = {
                    "loc": ["body"],
                    "msg": "JSON decode error",
                    "type": "json_invalid",
                }
        else:
            body_level_error = {
                "loc": ["body"],
                "msg": "Input should be a valid dictionary or object to extract fields from",
                "type": "model_attributes_type",
            }

    needs_body_for_name = name_q is None and "name" not in body_obj
    needs_body_for_age = age_q is None and "age" not in body_obj
    if body_level_error and (needs_body_for_name or needs_body_for_age):
        return None, None, [body_level_error]

    errors: list[dict] = []

    name_val = name_q if name_q is not None else body_obj.get("name")
    if name_val is None:
        errors.append(
            {"loc": ["body", "name"], "msg": "Field required", "type": "missing"}
        )

    age_val: int | None = None
    if age_q is not None:
        err = _pydantic_field_error(_INT_ADAPTER, age_q, ["query", "age"])
        if err:
            errors.append(err)
        else:
            age_val = _INT_ADAPTER.validate_python(age_q)
    else:
        raw_age = body_obj.get("age")
        if raw_age is None:
            errors.append(
                {"loc": ["body", "age"], "msg": "Field required", "type": "missing"}
            )
        else:
            err = _pydantic_field_error(_INT_ADAPTER, raw_age, ["body", "age"])
            if err:
                errors.append(err)
            else:
                age_val = _INT_ADAPTER.validate_python(raw_age)

    return name_val, age_val, errors


@app.post("/users", status_code=201)
async def create_user(
    request: Request,
    name: str | None = Query(None),
    age: str | None = Query(None),
) -> list[dict]:
    raw_body = await request.body()
    content_type = request.headers.get("content-type", "")
    name_val, age_val, errors = _resolve_create_user(name, age, raw_body, content_type)
    if errors:
        raise HTTPException(status_code=422, detail=errors)
    return [{"name": name_val, "age": age_val}]


@app.put("/items/{id}")
def put_item(id: int, q: str) -> list[dict]:
    return [{"id": id, "q": q}]


@app.patch("/items/{id}")
def patch_item(id: int) -> list[dict]:
    return [{"id": id}]


@app.delete("/items/{id}")
def delete_item(id: int) -> list[dict]:
    return [{"deleted_id": id}]


# routes.sql's `AS text` / `AS html` are quackapi's own magic column names:
# a single column literally named "text" or "html" renders as a raw
# text/plain or text/html body instead of the usual JSON array-of-rows
# (confirmed live: `curl /status/created` returns the bare string "created"
# with Content-Type: text/plain, not `[{"text":"created"}]`). status_created
# and status_teapot use `AS text`, so the reference must too.
@app.get("/status/created", status_code=201, response_class=PlainTextResponse)
def status_created() -> str:
    return "created"


@app.get("/status/teapot", status_code=418, response_class=PlainTextResponse)
def status_teapot() -> str:
    return "short"


@app.get("/status/nocontent", status_code=204, response_class=PlainTextResponse)
def status_nocontent() -> Response:
    return PlainTextResponse(content="", status_code=204)


@app.get("/page", response_class=HTMLResponse)
def page_html() -> str:
    return "<h1>hi</h1>"


@app.get("/plain", response_class=PlainTextResponse)
def page_text() -> str:
    return "hello"


@app.get("/json")
def page_json() -> list[dict]:
    return [{"msg": "world", "n": 42, "ok": True, "missing": None}]


def require_api_key(
    x_api_key: str | None = Header(default=None, alias="X-API-Key"),
) -> str:
    if x_api_key is None:
        raise HTTPException(
            status_code=401,
            detail="Not authenticated",
            headers={"WWW-Authenticate": "ApiKey"},
        )
    sub = API_KEYS.get(x_api_key)
    if sub is None:
        raise HTTPException(
            status_code=401, detail="Invalid authentication credentials"
        )
    return sub


@app.get("/secure")
def secure(sub: str = Depends(require_api_key)) -> list[dict]:
    return [{"ok": True, "sub": sub}]


def _b64url_decode(value: str) -> bytes:
    padded = value + "=" * (-len(value) % 4)
    return base64.urlsafe_b64decode(padded)


def require_jwt(authorization: str | None = Header(default=None)) -> dict:
    if not authorization or not authorization.startswith("Bearer "):
        raise HTTPException(
            status_code=401,
            detail="Not authenticated",
            headers={"WWW-Authenticate": "Bearer"},
        )
    token = authorization[len("Bearer ") :]
    parts = token.split(".")
    if len(parts) != 3:
        raise HTTPException(
            status_code=401, detail="Invalid authentication credentials"
        )
    header_b64, payload_b64, sig_b64 = parts
    expected_sig = (
        base64.urlsafe_b64encode(
            hmac.new(
                JWT_SECRET, f"{header_b64}.{payload_b64}".encode(), hashlib.sha256
            ).digest()
        )
        .rstrip(b"=")
        .decode()
    )
    if not hmac.compare_digest(expected_sig, sig_b64):
        raise HTTPException(
            status_code=401, detail="Invalid authentication credentials"
        )
    try:
        return json.loads(_b64url_decode(payload_b64))
    except (ValueError, UnicodeDecodeError) as exc:
        raise HTTPException(
            status_code=401, detail="Invalid authentication credentials"
        ) from exc


@app.get("/jwt")
def jwt_route(_payload: dict = Depends(require_jwt)) -> list[dict]:
    return [{"status": "ok"}]


@app.get("/header-echo")
def header_echo(x_token: str = Header(..., alias="X-Token")) -> list[dict]:
    return [{"token": x_token}]


@app.get("/profile")
def profile(session: str = Cookie(...)) -> list[dict]:
    return [{"session": session}]


@app.get("/old-home")
def old_home() -> RedirectResponse:
    return RedirectResponse(url="/new-home", status_code=307)


@app.post("/login")
def login(response: Response) -> list[dict]:
    response.set_cookie("session", "sess-abc", path="/", samesite=None)
    return [{"ok": True}]


@app.post("/form-submit")
async def form_submit(request: Request) -> list[dict]:
    form = await request.form()
    return [
        {"name": form.get("name"), "age": int(form["age"]) if "age" in form else None}
    ]


@app.post("/upload")
async def upload(request: Request) -> list[dict]:
    form = await request.form()
    file = form["file"]
    content = await file.read()
    return [
        {
            "content": content.decode("utf-8", errors="replace"),
            "filename": file.filename,
        }
    ]
