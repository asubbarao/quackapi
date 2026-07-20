"""Tiny FastAPI app used as a committed fixture for the quack_from_fastapi
sitting_duck extraction test (test/sql/quackapi_from_x_bridge.test).

Deliberately small: two routes (one with a path param, one with a Pydantic
body model) and one Pydantic model with a required + an optional field.
Mirrors the shape of the fastapi-realworld corpus repo used to prove the
one-caller in /tmp/quackapi_fromfast.md, just trimmed to fixture size.
"""

from fastapi import APIRouter, FastAPI
from pydantic import BaseModel

app = FastAPI()
router = APIRouter()


class UserInLogin(BaseModel):
    email: str
    password: str
    remember_me: bool = False


@router.get("/articles/{slug}")
def get_article(slug: str):
    return {"slug": slug}


@router.post("/login")
def login(user: UserInLogin):
    return {"handler": "login", "email": user.email}


app.include_router(router, prefix="/api")
