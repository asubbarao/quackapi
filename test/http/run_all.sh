#!/usr/bin/env bash
# Run every test/http/*.test.sh; exit non-zero on any failure.
set -euo pipefail

DIR="$(cd "$(dirname "$0")" && pwd)"
export DUCKDB_BIN="${DUCKDB_BIN:-$(cd "$DIR/../.." && pwd)/build/release/duckdb}"

if [[ ! -x "$DUCKDB_BIN" ]]; then
  echo "duckdb binary not found/executable: $DUCKDB_BIN" >&2
  echo "Build first: CMAKE_BUILD_PARALLEL_LEVEL=4 MAKEFLAGS=-j4 make release" >&2
  exit 2
fi

shopt -s nullglob
tests=("$DIR"/*.test.sh)
if [[ ${#tests[@]} -eq 0 ]]; then
  echo "No test/http/*.test.sh found" >&2
  exit 1
fi

failed=0
passed=0
for t in "${tests[@]}"; do
  name="$(basename "$t")"
  echo "=== RUN $name ==="
  if bash "$t"; then
    echo "=== PASS $name ==="
    passed=$((passed + 1))
  else
    echo "=== FAIL $name ===" >&2
    failed=$((failed + 1))
  fi
done

echo "http tests: $passed passed, $failed failed"
if [[ "$failed" -ne 0 ]]; then
  exit 1
fi
exit 0
