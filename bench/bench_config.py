"""Connection configuration shared by benchmark launchers and the FastAPI app."""

import os
from urllib.parse import quote


def pg_dsn() -> str:
    explicit = os.environ.get("PG_DSN")
    if explicit:
        return explicit
    host = os.environ.get("PG_HOST", "127.0.0.1")
    if ":" in host and not host.startswith("["):
        host = f"[{host}]"
    user = quote(os.environ.get("PG_USER", "admin"), safe="")
    password = quote(os.environ.get("PGPASSWORD", "password"), safe="")
    database = quote(os.environ.get("PG_DB", "quackbench"), safe="")
    port = os.environ.get("PG_PORT", "6432")
    return f"postgresql://{user}:{password}@{host}:{port}/{database}"


if __name__ == "__main__":
    print(pg_dsn())
