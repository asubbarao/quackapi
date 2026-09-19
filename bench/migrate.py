#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
from pathlib import Path
import subprocess


def sql_literal(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def extract(duckdb: Path, extension: Path, source: Path, function: str) -> list[dict]:
    statement = (
        f"LOAD {sql_literal(str(extension))}; "
        f"FROM {function}({sql_literal(str(source))});"
    )
    completed = subprocess.run(
        [str(duckdb), "-no-init", "-unsigned", "-json", ":memory:", "-c", statement],
        check=True,
        text=True,
        capture_output=True,
    )
    return json.loads(completed.stdout)


def route_specs() -> list[dict[str, str]]:
    return [
        {
            "handler": "root", "method": "GET", "local_path": "/", "file_suffix": "/main.py",
            "sql": """CREATE ROUTE migrated_root GET '/'
  ENVELOPE object
  EMPTY STATUS 400 BODY '{"detail":"No Jessica token provided"}'
  PARAM token VARCHAR
  AS SELECT 'Hello Bigger Applications!' AS message
  WHERE $token = 'jessica';""",
        },
        {
            "handler": "read_users", "method": "GET", "local_path": "/users/", "file_suffix": "/routers/users.py",
            "sql": """CREATE ROUTE migrated_users GET '/users/'
  EMPTY STATUS 400 BODY '{"detail":"No Jessica token provided"}'
  PARAM token VARCHAR
  AS SELECT username FROM (VALUES ('Rick'), ('Morty')) users(username)
  WHERE $token = 'jessica';""",
        },
        {
            "handler": "read_user_me", "method": "GET", "local_path": "/users/me", "file_suffix": "/routers/users.py",
            "sql": """CREATE ROUTE migrated_user_me GET '/users/me'
  ENVELOPE object
  EMPTY STATUS 400 BODY '{"detail":"No Jessica token provided"}'
  PARAM token VARCHAR
  AS SELECT 'fakecurrentuser' AS username
  WHERE $token = 'jessica';""",
        },
        {
            "handler": "read_user", "method": "GET", "local_path": "/users/{username}", "file_suffix": "/routers/users.py",
            "sql": """CREATE ROUTE migrated_user GET '/users/:username'
  ENVELOPE object
  EMPTY STATUS 400 BODY '{"detail":"No Jessica token provided"}'
  PARAM token VARCHAR
  AS SELECT $username::VARCHAR AS username
  WHERE $token = 'jessica';""",
        },
        {
            "handler": "read_item", "method": "GET", "local_path": "/{item_id}", "file_suffix": "/routers/items.py",
            "sql": """CREATE ROUTE migrated_item GET '/items/:item_id'
  ENVELOPE object
  EMPTY STATUS 404 BODY '{"detail":"Item not found"}'
  PARAM token VARCHAR
  PARAM x_token VARCHAR HEADER
  AS SELECT CASE $item_id WHEN 'plumbus' THEN 'Plumbus' ELSE 'Portal Gun' END AS name,
            $item_id::VARCHAR AS item_id
  WHERE $token = 'jessica'
    AND $x_token = 'fake-super-secret-token'
    AND $item_id IN ('plumbus', 'gun');""",
        },
        {
            "handler": "update_item", "method": "PUT", "local_path": "/{item_id}", "file_suffix": "/routers/items.py",
            "sql": """CREATE ROUTE migrated_update_item PUT '/items/:item_id'
  ENVELOPE object
  EMPTY STATUS 403 BODY '{"detail":"You can only update the item: plumbus"}'
  PARAM token VARCHAR
  PARAM x_token VARCHAR HEADER
  AS SELECT $item_id::VARCHAR AS item_id, 'The great Plumbus' AS name
  WHERE $token = 'jessica'
    AND $x_token = 'fake-super-secret-token'
    AND $item_id = 'plumbus';""",
        },
        {
            "handler": "update_admin", "method": "POST", "local_path": "/", "file_suffix": "/internal/admin.py",
            "sql": """CREATE ROUTE migrated_admin POST '/admin/'
  ENVELOPE object
  EMPTY STATUS 400 BODY '{"detail":"X-Token header invalid"}'
  PARAM token VARCHAR
  PARAM x_token VARCHAR HEADER
  AS SELECT 'Admin getting schwifty' AS message
  WHERE $token = 'jessica' AND $x_token = 'fake-super-secret-token';""",
        },
    ]


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--duckdb", type=Path, required=True)
    parser.add_argument("--extension", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    args = parser.parse_args()

    routes = extract(args.duckdb, args.extension, args.source, "quack_from_fastapi")
    models = extract(args.duckdb, args.extension, args.source, "quack_from_fastapi_models")
    specs = route_specs()
    matched: list[dict] = []
    for spec in specs:
        candidates = [
            route for route in routes
            if route["handler_name"] == spec["handler"]
            and route["method"] == spec["method"]
            and route["path"] == spec["local_path"]
            and route["file"].endswith(spec["file_suffix"])
        ]
        if len(candidates) != 1:
            raise SystemExit(
                f"expected exactly one extracted route for {spec['handler']}, got {len(candidates)}"
            )
        matched.append({"extracted": candidates[0], "manual_sql": spec["sql"]})

    args.output_dir.mkdir(parents=True, exist_ok=True)
    extraction = {
        "source": str(args.source),
        "routes": routes,
        "models": models,
        "matched_routes": matched,
        "manual_work": [
            "resolved APIRouter/include_router prefixes",
            "recreated application and header dependencies",
            "translated selected Python handler bodies to SQL",
            "selected JSON envelope and empty-result status behavior",
            "added a benchmark-only durability endpoint to both services",
        ],
    }
    (args.output_dir / "extraction.json").write_text(json.dumps(extraction, indent=2) + "\n")

    generated = [
        f"LOAD {sql_literal(str(args.extension))};",
        "CREATE OR REPLACE QUEUE bench_jobs WITH (max_attempts=3, visibility_timeout='30s');",
    ]
    generated.extend(spec["sql"] for spec in specs)
    generated.append(
        """CREATE ROUTE benchmark_durable_job POST '/bench/jobs'
  STATUS 202
  ENVELOPE object
  AS SELECT $id::BIGINT AS id,
            quackapi_enqueue('bench_jobs', json_object('id', $id, 'delay_ms', $delay_ms)) AS job_id;"""
    )
    (args.output_dir / "generated_routes.sql").write_text("\n\n".join(generated) + "\n")
    print(json.dumps({"routes": len(routes), "models": len(models), "mapped": len(specs)}))


if __name__ == "__main__":
    main()
