"""with_router.py — APIRouter with prefix, include_router with additional prefix."""
from fastapi import FastAPI, APIRouter
from pydantic import BaseModel

app = FastAPI()

# Router defined with its own prefix
items_router = APIRouter(prefix="/items")
users_router = APIRouter()


class UserBody(BaseModel):
    name: str
    age: int


@items_router.get("/")
def list_items():
    return []


@items_router.get("/{item_id}")
def get_item(item_id: int):
    return {"id": item_id}


@items_router.post("/")
def create_item(name: str, price: float):
    return {"name": name, "price": price}


@users_router.get("/{user_id}")
def get_user(user_id: int):
    return {"id": user_id}


@users_router.post("/")
def create_user(body: UserBody):
    return body


# include_router adds an additional prefix on top of the router's own prefix
app.include_router(items_router)
app.include_router(users_router, prefix="/users")
