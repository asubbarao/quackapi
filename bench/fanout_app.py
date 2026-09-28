"""FastAPI async fan-out stack: one request -> N concurrent outbound calls via httpx."""
from __future__ import annotations
import asyncio
import httpx
from fastapi import FastAPI, HTTPException, Query

app = FastAPI()
client: httpx.AsyncClient | None = None

@app.on_event("startup")
async def _startup() -> None:
    global client
    # generous connection pool so the client isn't the bottleneck
    client = httpx.AsyncClient(limits=httpx.Limits(max_connections=1000, max_keepalive_connections=1000))

@app.on_event("shutdown")
async def _shutdown() -> None:
    if client is not None:
        await client.aclose()

@app.get("/fanout")
async def fanout(
    n: int = Query(10, ge=1, le=64),
    ms: int = Query(50, ge=0, le=30_000),
):
    assert client is not None
    import random
    nz = random.randrange(10**9)
    urls = [f"http://127.0.0.1:9000/slow?ms={ms}&id={i}&nz={nz}_{i}" for i in range(n)]
    try:
        resps = await asyncio.gather(*[client.get(u) for u in urls])
        if any(r.status_code < 200 or r.status_code >= 300 for r in resps):
            raise HTTPException(status_code=502, detail="fan-out upstream returned an error")
        data = [r.json() for r in resps]
    except (httpx.HTTPError, ValueError) as exc:
        raise HTTPException(status_code=502, detail="fan-out upstream request failed") from exc
    if any(not isinstance(d, dict) or d.get("id") != i or d.get("ok") is not True for i, d in enumerate(data)):
        raise HTTPException(status_code=502, detail="fan-out upstream response is invalid")
    return {"n": len(data), "sum_ids": sum(d["id"] for d in data)}
