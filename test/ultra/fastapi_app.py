"""FastAPI reference application for the paired QuackAPI matrix.

The application is deliberately small and deterministic.  It exercises the
framework contracts that can be represented by SQL routes: typed path/query
parameters, Pydantic request and response models, auth, headers/cookies,
content negotiation, redirects, CORS, compression, streaming, and errors.
"""

from __future__ import annotations

import json
from typing import Annotated, Any

from fastapi import Cookie, FastAPI, Header, HTTPException, Query, Request, Response, status
from fastapi.middleware.cors import CORSMiddleware
from fastapi.middleware.gzip import GZipMiddleware
from fastapi.responses import HTMLResponse, JSONResponse, PlainTextResponse, RedirectResponse, StreamingResponse
from pydantic import BaseModel, ConfigDict, Field


class ItemIn(BaseModel):
    model_config = ConfigDict(extra="forbid")

    name: str = Field(min_length=1, max_length=32)
    price: float = Field(ge=0, le=10_000)
    tags: list[str] = Field(default_factory=list, max_length=4)


class ItemOut(BaseModel):
    id: int
    name: str
    price: float
    tags: list[str]


ITEMS: dict[int, ItemOut] = {
    1: ItemOut(id=1, name="alice", price=12.5, tags=["staff"]),
    2: ItemOut(id=2, name="bob", price=8.0, tags=["customer"]),
    3: ItemOut(id=3, name="carol", price=20.0, tags=["staff", "admin"]),
}

app = FastAPI(title="QuackAPI ultra parity fixture", version="1.0")
app.add_middleware(GZipMiddleware, minimum_size=256)
app.add_middleware(
    CORSMiddleware,
    allow_origins=["http://example.test"],
    allow_methods=["GET", "POST", "HEAD", "OPTIONS"],
    allow_headers=["Authorization", "Content-Type", "X-Token"],
)


@app.get("/matrix/health", response_model=dict[str, str])
def health() -> dict[str, str]:
    return {"status": "ok"}


@app.head("/matrix/health")
def health_head() -> Response:
    return Response(status_code=200)


@app.get("/matrix/items/{item_id}", response_model=ItemOut)
def item(item_id: int) -> ItemOut:
    found = ITEMS.get(item_id)
    if found is None:
        raise HTTPException(status_code=404, detail="Not Found")
    return found


@app.post("/matrix/items", response_model=ItemOut, status_code=201)
def create_item(body: ItemIn) -> ItemOut:
    # A stable id keeps the paired comparison deterministic across requests.
    return ItemOut(id=100, name=body.name, price=body.price, tags=body.tags)


@app.get("/matrix/search", response_model=list[ItemOut])
def search(
    q: Annotated[str, Query(min_length=1, max_length=40)],
    limit: Annotated[int, Query(ge=0, le=100)] = 10,
) -> list[ItemOut]:
    needle = q.casefold()
    return [item for item in ITEMS.values() if needle in item.name.casefold()][:limit]


@app.get("/matrix/headers")
def headers(
    x_token: Annotated[str | None, Header(alias="X-Token")] = None,
    session: str | None = Cookie(default=None),
) -> dict[str, Any]:
    return {"token": x_token, "session": session}


@app.get("/matrix/auth")
def auth(request: Request) -> dict[str, str]:
    value = request.headers.get("authorization")
    if value is None or not value.startswith("Bearer ") or value[7:] == "":
        raise HTTPException(status_code=401, detail="Not authenticated", headers={"WWW-Authenticate": "Bearer"})
    if value[7:] != "matrix-secret":
        # API-key style authentication treats an unrecognized credential as
        # unauthenticated.  Keeping this at 401 makes the paired fixture
        # compare the same contract as QuackAPI's REQUIRE API_KEY route.
        raise HTTPException(
            status_code=401, detail="Invalid authentication credentials", headers={"WWW-Authenticate": "Bearer"}
        )
    return {"subject": "matrix-user"}


@app.get("/matrix/redirect")
def redirect() -> RedirectResponse:
    return RedirectResponse("/matrix/health", status_code=307)


@app.get("/matrix/html", response_class=HTMLResponse)
def html() -> str:
    return "<h1>quack</h1>"


@app.get("/matrix/text", response_class=PlainTextResponse)
def text() -> str:
    return "quack"


@app.get("/matrix/object/{item_id}", response_model=ItemOut)
def object_item(item_id: int) -> ItemOut:
    found = ITEMS.get(item_id)
    if found is None:
        raise HTTPException(status_code=404, detail={"code": "missing"})
    return found


@app.get("/matrix/empty/{item_id}")
def empty_item(item_id: int) -> JSONResponse:
    if item_id in ITEMS:
        return JSONResponse(ITEMS[item_id].model_dump())
    return JSONResponse({"detail": "item not found"}, status_code=404)


@app.get("/matrix/response-filter", response_model=ItemOut)
def response_filter() -> dict[str, Any]:
    return {**ITEMS[1].model_dump(), "secret": "must-not-leak"}


@app.get("/matrix/compressed")
def compressed() -> dict[str, str]:
    return {"payload": "x" * 4096}


@app.get("/matrix/ndjson")
def ndjson() -> StreamingResponse:
    def rows():
        for item in ITEMS.values():
            yield json.dumps(item.model_dump(), separators=(",", ":")) + "\n"

    return StreamingResponse(rows(), media_type="application/x-ndjson")


@app.get("/matrix/stream")
def stream() -> StreamingResponse:
    def events():
        for i in range(3):
            yield f"event: row\ndata: {json.dumps({'id': i})}\n\n"

    return StreamingResponse(events(), media_type="text/event-stream")


@app.get("/matrix/csv")
def csv() -> Response:
    return Response("id,name\n1,alice\n2,bob\n", media_type="text/csv")


@app.get("/matrix/problem")
def problem() -> None:
    raise HTTPException(status_code=status.HTTP_418_IM_A_TEAPOT, detail="short")
