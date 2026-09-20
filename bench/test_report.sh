#!/usr/bin/env bash
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "$0")" && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${BENCH_DIR}/../build/release/duckdb}"
fixture="$(mktemp -d /tmp/quackapi-honest-report.XXXXXX)"
trap 'rm -rf "${fixture}"' EXIT

printf '%s\n' \
  '{"stack":"quackapi","trial":1,"concurrency":32,"attempted":96,"successful":92,"successful_rps":100,"p50_ms":1,"p99_ms":9,"max_ms":10028,"contract_failures":0,"shed":6,"timeouts":0,"resets":4,"eofs":0,"other_no_response":0}' \
  '{"stack":"fastapi","trial":1,"concurrency":32,"attempted":90,"successful":90,"successful_rps":90,"p50_ms":2,"p99_ms":10,"max_ms":12,"contract_failures":0,"shed":0,"timeouts":0,"resets":0,"eofs":0,"other_no_response":0}' \
  >"${fixture}/measurements.jsonl"
printf '%s\n' \
  '{"stack":"quackapi","passed":8,"total":9}' \
  '{"stack":"fastapi","passed":9,"total":9}' \
  >"${fixture}/conformance.jsonl"
printf '%s\n' \
  '{"stack":"quackapi","attempted":64,"acknowledged":64,"survived":64}' \
  '{"stack":"fastapi","attempted":64,"acknowledged":64,"survived":0}' \
  >"${fixture}/crash.jsonl"

(
  cd "${fixture}"
  "${DUCKDB_BIN}" -no-init -csv <"${BENCH_DIR}/report.sql" >report.csv
  grep -F -q '10028' report.csv
  grep -F -q 'contract_failures,shed,' report.csv
  grep -F -q '64,64,64,0' report.csv
)

echo "PASS: report preserves max latency and crash survivors"
