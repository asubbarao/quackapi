"""hard.py — cases the decorator scan CANNOT fully handle.

Intentionally adversarial:
  1. IMPORTED model (ExternalModel comes from another module — body param type
     is not defined in this file, so field extraction is impossible).
  2. Dynamic registration via app.add_api_route(...) — invisible to decorator scan.
  3. Class-based view (CBV) pattern via APIRouter + __init_subclass__ trick —
     decorated methods on a class, not a plain function.
  4. app.mount(...) sub-application — a whole sub-app, not a route.
"""
from fastapi import FastAPI, APIRouter
from pydantic import BaseModel

# Simulated import of an external model (defined elsewhere — NOT in this file)
from myapp.models import ExternalModel   # noqa: F401 — intentionally unresolvable

app = FastAPI()


class LocalModel(BaseModel):
    title: str
    count: int = 0


# ── Case 1: imported body model ────────────────────────────────────────────
@app.post("/submit")
def submit(payload: ExternalModel):
    """Body type is ExternalModel — imported, not locally defined."""
    return payload


# ── Case 2: a normal decorated route (should migrate cleanly) ──────────────
@app.get("/status")
def status():
    return {"ok": True}


@app.post("/local")
def create_local(body: LocalModel):
    """LocalModel IS defined in this file — should migrate."""
    return body


# ── Case 3: dynamic registration (invisible to decorator scan) ─────────────
def dynamic_handler(item_id: int):
    return {"id": item_id}

app.add_api_route("/dynamic/{item_id}", dynamic_handler, methods=["GET"])

# Another dynamic registration — list endpoint
app.add_api_route("/dynamic", dynamic_handler, methods=["GET", "POST"])


# ── Case 4: class-based view (methods on a class, not plain functions) ──────
class ItemView:
    @staticmethod
    @app.get("/cbv/items")
    def list_items():
        return []

    @staticmethod
    @app.post("/cbv/items")
    def create_item(name: str):
        return {"name": name}


# ── Case 5: sub-application mount ─────────────────────────────────────────
sub_app = FastAPI()

@sub_app.get("/ping")
def sub_ping():
    return {"pong": True}

app.mount("/sub", sub_app)
