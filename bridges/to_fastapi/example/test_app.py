"""The generated FastAPI route serves what the quackapi route served for the same URL.

quackapi_response.json is the quackapi route's body for that URL. Amounts are compared as decimals and
timestamps as datetimes: FastAPI writes Decimal as a string and ISO timestamps, quackapi numbers and spaces.

Run: uv run --with fastapi --with httpx --with duckdb --with pytest pytest test_app.py
"""

import json
from datetime import datetime
from decimal import Decimal
from pathlib import Path

from app.example_orders_route import router
from fastapi import FastAPI
from fastapi.testclient import TestClient

URL = "/customers/00000000-0000-0000-0000-00000000000a/orders?since=2026-01-01"
AMOUNTS = {"total", "unit_price"}


def normalized(value, key=None):
    if isinstance(value, dict):
        return {k: normalized(v, k) for k, v in value.items()}
    if isinstance(value, list):
        return [normalized(v) for v in value]
    if value is not None and key in AMOUNTS:
        return Decimal(str(value))
    if value is not None and key == "placed_at":
        return datetime.fromisoformat(value)
    return value


def test_generated_route_serves_the_quackapi_body() -> None:
    app = FastAPI()
    app.include_router(router)
    response = TestClient(app).get(URL)
    assert response.status_code == 200
    quackapi = json.loads(
        (Path(__file__).parent / "quackapi_response.json").read_text()
    )
    assert normalized(response.json()) == normalized(quackapi)


def test_generated_route_refuses_a_bad_parameter() -> None:
    app = FastAPI()
    app.include_router(router)
    response = TestClient(app).get("/customers/not-a-uuid/orders?since=2026-01-01")
    assert response.status_code == 422
