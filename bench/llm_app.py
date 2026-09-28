"""FastAPI as an LLM API gateway -- stack B for the ollama/pgEdge suite.

Mirrors bench/llm_routes.sql exactly: inbound request -> call ollama -> durably
log the VERBATIM upstream body -> respond. The default mode awaits the pooled
asyncpg insert so it matches quackapi's durable enqueue-before-response
contract; an explicitly selected ``LLM_DURABILITY_MODE=deferred`` mode uses
BackgroundTasks for a lower-latency but lossy comparison.

BackgroundTasks is NOT durable. If the process dies between responding and
draining, those log rows are gone -- there is no on-disk job record to retry
from. quackapi's queue is a WAL-backed table, so the same crash is recoverable.
The benchmark must label deferred and durable modes separately; durability is a
property, not a latency number.
"""
from __future__ import annotations

import json
import os

import asyncpg
import httpx
from fastapi import BackgroundTasks, FastAPI, HTTPException

OLLAMA = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
PG_DSN = os.environ.get(
    "PG_DSN", "postgresql://admin:password@127.0.0.1:6432/example"
)

app = FastAPI()
client: httpx.AsyncClient | None = None
pool: asyncpg.Pool | None = None
DURABILITY_MODE = os.environ.get("LLM_DURABILITY_MODE", "durable")

INSERT_SQL = """
INSERT INTO llm_calls (host, source, api, model, request, raw)
VALUES ($1, $2, $3, $4, $5::jsonb, $6::jsonb)
"""


@app.on_event("startup")
async def _startup() -> None:
    global client, pool
    client = httpx.AsyncClient(
        timeout=600.0,
        limits=httpx.Limits(max_connections=1000, max_keepalive_connections=1000),
    )
    pool = await asyncpg.create_pool(PG_DSN, min_size=4, max_size=32)


@app.on_event("shutdown")
async def _shutdown() -> None:
    if client is not None:
        await client.aclose()
    if pool is not None:
        await pool.close()


async def _log(api: str, model: str, request: dict, raw: str) -> None:
    assert pool is not None
    async with pool.acquire() as conn:
        await conn.execute(
            INSERT_SQL, "localhost", "fastapi-bench", api, model,
            json.dumps(request), raw,
        )


async def _record(bg: BackgroundTasks, api: str, model: str, request: dict, raw: str) -> None:
    # The default matches quackapi's durable enqueue-before-response contract.
    # Deferred logging is available for an explicitly named, lower-latency mode
    # but must never be presented as an apples-to-apples durability comparison.
    if DURABILITY_MODE == "deferred":
        bg.add_task(_log, api, model, request, raw)
    else:
        await _log(api, model, request, raw)


@app.post("/llm/embed")
async def embed(bg: BackgroundTasks, model: str, prompt: str):
    assert client is not None
    req = {"model": model, "prompt": prompt}
    try:
        r = await client.post(f"{OLLAMA}/api/embeddings", json=req)
        r.raise_for_status()
        d = r.json()
    except (httpx.HTTPError, ValueError) as exc:
        raise HTTPException(status_code=502, detail="ollama embedding request failed") from exc
    embedding = d.get("embedding") if isinstance(d, dict) else None
    if not isinstance(embedding, list) or not embedding:
        raise HTTPException(status_code=502, detail="ollama embedding response is invalid")
    raw = r.text
    await _record(bg, "embeddings", model, req, raw)
    return {"dims": len(embedding)}


@app.post("/llm/ask")
async def ask(bg: BackgroundTasks, model: str, prompt: str, num_predict: int = 16):
    assert client is not None
    req = {"model": model, "prompt": prompt}
    try:
        r = await client.post(
            f"{OLLAMA}/api/generate",
            json={**req, "stream": False, "options": {"num_predict": num_predict}},
        )
        r.raise_for_status()
        d = r.json()
    except (httpx.HTTPError, ValueError) as exc:
        raise HTTPException(status_code=502, detail="ollama generation request failed") from exc
    raw = r.text
    if not isinstance(d, dict) or not isinstance(d.get("response"), str):
        raise HTTPException(status_code=502, detail="ollama generation response is invalid")
    total_duration = d.get("total_duration")
    if not isinstance(total_duration, (int, float)) or total_duration <= 0:
        raise HTTPException(status_code=502, detail="ollama generation timing is invalid")
    await _record(bg, "generate", model, req, raw)
    return {
        "response": d.get("response"),
        "ollama_total_ms": total_duration / 1e6,
        "out_tokens": d.get("eval_count"),
    }
