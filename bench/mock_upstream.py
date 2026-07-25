"""High-concurrency async mock upstream for fan-out benchmarking.
GET /slow?id=<n>&ms=<latency> -> sleeps <ms> then returns small JSON.
async def + asyncio.sleep => thousands of concurrent in-flight requests cheaply,
so the UPSTREAM is never the bottleneck; the client's fan-out model is what's measured.
"""
from __future__ import annotations
import asyncio
from starlette.applications import Starlette
from starlette.responses import JSONResponse
from starlette.routing import Route

async def slow(request):
    q = request.query_params
    ms = int(q.get("ms", "50"))
    ident = int(q.get("id", "0"))
    await asyncio.sleep(ms / 1000.0)
    return JSONResponse({"id": ident, "ok": True, "ms": ms})

app = Starlette(routes=[Route("/slow", slow)])
