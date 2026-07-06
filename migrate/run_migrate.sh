#!/usr/bin/env bash
# run_migrate.sh — FastAPI → quackapi migration runner
#
# Usage:
#   bash migrate/run_migrate.sh path/to/app.py
#   bash migrate/run_migrate.sh 'path/to/repo/**/*.py'
#
# Output:
#   Coverage report (MIGRATED / NEEDS_REVIEW / NOT_DETECTED) to stderr
#   Raw output (registration SQL + coverage) to stdout as CSV
#
# See migrate/README.md for full documentation.

set -euo pipefail

if [[ $# -lt 1 ]]; then
  echo "Usage: bash migrate/run_migrate.sh <glob_or_file>" >&2
  echo "  Example: bash migrate/run_migrate.sh 'myapp/**/*.py'" >&2
  exit 1
fi

SOURCE_GLOB="$1"
DUCKDB="${DUCKDB:-/opt/homebrew/bin/duckdb}"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TMPDB="/tmp/qmig_run_$$.db"

cleanup() { rm -f "$TMPDB" "${TMPDB}.wal" 2>/dev/null || true; }
trap cleanup EXIT

echo "=== quackapi FastAPI Migration Tool ===" >&2
echo "Source:  $SOURCE_GLOB" >&2
echo "" >&2

# Export source path as environment variable so migrate_fastapi.sql can read it
# via getenv('QUACKAPI_SRC') — a launch-time immutable, never a session variable.
export QUACKAPI_SRC="$SOURCE_GLOB"

# Run migration + coverage in one DuckDB session
"$DUCKDB" -unsigned -csv "$TMPDB" 2>&1 <<ENDSQL
INSTALL sitting_duck FROM community; LOAD sitting_duck;
$(cat "$SCRIPT_DIR/migrate_fastapi.sql")
$(cat "$SCRIPT_DIR/COVERAGE.sql")
ENDSQL

echo "" >&2
echo "Done. The output above contains:" >&2
echo "  - registration_sql column: quackapi INSERT statements (write SQL handlers)" >&2
echo "  - status/route/handler_name columns: MIGRATED / NEEDS_REVIEW / NOT_DETECTED" >&2
