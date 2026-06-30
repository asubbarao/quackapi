#!/usr/bin/env bash
# Boot the quackapi unified server (Tier 2 — browser-native HTTP).
# Run from the repo root:  ./run.sh
#
# The CLI opens quackapi.db and loads the framework + demo app + C server;
# serve_brain() then serves that same database on port 18099. Override the
# duckdb binary with DUCKDB=/path/to/duckdb ./run.sh
set -euo pipefail
cd "$(dirname "$0")"
DUCK="${DUCKDB:-duckdb}"
echo "quackapi  →  http://127.0.0.1:18099    (Swagger UI at /docs · Ctrl-C to stop)"
exec "$DUCK" quackapi.db < launch_server.sql
