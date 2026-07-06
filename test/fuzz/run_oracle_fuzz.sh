#!/usr/bin/env bash
# =============================================================================
# run_oracle_fuzz.sh — runs oracle_fuzz.test.sql against handle_request()
# No HTTP server needed. No build needed. Pure SQL oracle only.
#
# Canonical invocation:
#   bash test/fuzz/run_oracle_fuzz.sh
# from the quackapi root directory.
#
# Reproduce command for a single run:
#   printf '.read framework.sql\n.read app.sql\n.read test/fuzz/oracle_fuzz.test.sql\n' \
#     | /opt/homebrew/bin/duckdb -unsigned
# =============================================================================
set -euo pipefail

DUCKDB=/opt/homebrew/bin/duckdb
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

cd "${ROOT}"

echo "=== quackapi oracle fuzz suite ==="
echo "    duckdb: ${DUCKDB}"
echo "    root:   ${ROOT}"
echo ""

# Run via stdin pipe to avoid temp files.
# The SQL prints all results then a summary row.
OUTPUT=$(printf '.read framework.sql\n.read app.sql\n.read test/fuzz/oracle_fuzz.test.sql\n' \
  | "${DUCKDB}" -unsigned 2>&1)

# Extract the last two table-formatted blocks from DuckDB output.
# The test file ends with:
#   1) a SELECT of all check results (pass ASC, check_name)
#   2) a summary SELECT of total/passed/failed
echo "${OUTPUT}" | tail -n 40

# Parse the summary line (last data line of summary block)
SUMMARY=$(echo "${OUTPUT}" | grep -E '^\s*[0-9]+\s*\|\s*[0-9]+\s*\|\s*[0-9]+\s*$' | tail -n 1)
if [ -z "${SUMMARY}" ]; then
  echo ""
  echo "ERROR: could not parse summary row from output."
  echo "--- full output ---"
  echo "${OUTPUT}"
  exit 2
fi

TOTAL=$(echo "${SUMMARY}"  | awk -F'|' '{gsub(/ /,"",$1); print $1}')
PASSED=$(echo "${SUMMARY}" | awk -F'|' '{gsub(/ /,"",$2); print $2}')
FAILED=$(echo "${SUMMARY}" | awk -F'|' '{gsub(/ /,"",$3); print $3}')

echo ""
echo "=== RESULT: ${PASSED}/${TOTAL} passed, ${FAILED} failed ==="

if [ "${FAILED}" -gt 0 ]; then
  echo ""
  echo "FAILING CHECKS:"
  echo "${OUTPUT}" | grep -E '\|\s*false\s*\|' || true
  exit 1
fi

exit 0
