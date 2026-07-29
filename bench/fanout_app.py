"""FastAPI async fan-out stack: one request -> N concurrent outbound calls via httpx."""
from __future__ import annotations
import asyncio
import httpx
from fastapi import FastAPI

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
async def fanout(n: int = 10, ms: int = 50):
    assert client is not None
    import random
    nz = random.randrange(10**9)
    urls = [f"http://127.0.0.1:9000/slow?ms={ms}&id={i}&nz={nz}_{i}" for i in range(n)]
    resps = await asyncio.gather(*[client.get(u) for u in urls])
    data = [r.json() for r in resps]
    return {"n": len(data), "sum_ids": sum(d["id"] for d in data)}
