from typing import Optional

from pydantic import BaseModel, Field


class Requiredness(BaseModel):
    nullable: Optional[str]
    nullable_default: Optional[str] = None
    explicit: str = Field(...)
    factory: list[str] = Field(default_factory=list)
    bare: str = Field()
    metadata: str = Field(description="Required text")
    named_ellipsis: str = Field(description="Required text", default=...)
    named_default: str = Field(description="Optional text", default="hello")
    positional_default: str = Field("hello", description="Optional text")
