"""simple.py — plain FastAPI decorators, no routers, no imports of external models."""
from fastapi import FastAPI
from pydantic import BaseModel

app = FastAPI()


class CreateItemBody(BaseModel):
    name: str
    price: float
    in_stock: bool = True


@app.get("/")
def root():
    return {"message": "hello"}


@app.get("/items/{item_id}")
def get_item(item_id: int, verbose: bool = False):
    return {"id": item_id, "verbose": verbose}


@app.get("/search")
def search_items(q: str, limit: int = 10, offset: int = 0):
    return []


@app.post("/items")
def create_item(body: CreateItemBody):
    return body


@app.put("/items/{item_id}")
def update_item(item_id: int, body: CreateItemBody):
    return {"id": item_id, **body.dict()}


@app.delete("/items/{item_id}")
def delete_item(item_id: int):
    return {"deleted": item_id}
