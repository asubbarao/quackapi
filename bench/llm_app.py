"""FastAPI as an LLM API gateway -- stack B for the ollama/pgEdge suite.

Mirrors bench/llm_routes.sql exactly: inbound request -> call ollama -> durably
log the VERBATIM upstream body -> respond, with logging kept off the response
path. FastAPI's idiomatic way to do that is BackgroundTasks + a pooled asyncpg
insert into the same pgEdge sink quackapi's drainer writes to.

Worth naming, because it is the architectural difference the suite exists to
show: BackgroundTasks is NOT durable. If the process dies between responding and
draining, those log rows are gone -- there is no on-disk job record to retry
from. quackapi's queue is a WAL-backed table, so the same crash is recoverable.
Both stacks are measured on latency here; durability is a property, not a number.
"""
from __future__ import annotations

import json
import os

import asyncpg
import httpx
from fastapi import BackgroundTasks, FastAPI

OLLAMA = os.environ.get("OLLAMA_URL", "http://127.0.0.1:11434")
PG_DSN = os.environ.get(
    "PG_DSN", "postgresql://admin:password@127.0.0.1:6432/example"
)

app = FastAPI()
client: httpx.AsyncClient | None = None
pool: asyncpg.Pool | None = None

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


@app.post("/llm/embed")
async def embed(bg: BackgroundTasks, model: str, prompt: str):
    assert client is not None
    req = {"model": model, "prompt": prompt}
    r = await client.post(f"{OLLAMA}/api/embeddings", json=req)
    raw = r.text
    bg.add_task(_log, "embeddings", model, req, raw)
    return {"dims": len(json.loads(raw).get("embedding") or [])}


@app.post("/llm/ask")
async def ask(bg: BackgroundTasks, model: str, prompt: str, num_predict: int = 16):
    assert client is not None
    req = {"model": model, "prompt": prompt}
    r = await client.post(
        f"{OLLAMA}/api/generate",
        json={**req, "stream": False, "options": {"num_predict": num_predict}},
    )
    raw = r.text
    d = json.loads(raw)
    bg.add_task(_log, "generate", model, req, raw)
    return {
        "response": d.get("response"),
        "ollama_total_ms": (d.get("total_duration") or 0) / 1e6,
        "out_tokens": d.get("eval_count"),
    }
