#!/usr/bin/env bash
# Reproducible report smoke test. Uses only generated k6-shaped rows.
set -euo pipefail

BENCH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DUCKDB_BIN="${DUCKDB_BIN:-${BENCH_DIR}/../build/release/duckdb}"
if [[ ! -x "$DUCKDB_BIN" ]]; then
  echo "SKIP: duckdb CLI not found: $DUCKDB_BIN"
  exit 0
fi

fixture="$(mktemp -d /tmp/quackapi-report.XXXXXX)"
trap 'rm -rf "$fixture"' EXIT
mkdir -p "$fixture/raw"

cat >"$fixture/cells.tsv" <<'EOF'
export_name	stack	scenario	vus	k6_exit	measure_requests	measure_successful	measure_http_failures	measure_check_failures	valid	invalid_reason	all_requests	all_successful	all_http_failures	measure_seconds
good__item__vus1	quackapi-w1	item	1	0	3	3	0	0	1		3	3	0	1
bad__item__vus1	fastapi-w1	item	1	0	3	3	0	1	0	measure_check_failures=1	3	3	0	1
EOF

{
  printf '%s\n' 'metric_name,metric_value,scenario'
  printf '%s\n' 'http_req_duration,1,measure' 'http_req_duration,2,measure' 'http_req_duration,3,measure'
} | gzip >"$fixture/raw/good__item__vus1.csv.gz"
{
  printf '%s\n' 'metric_name,metric_value,scenario'
  printf '%s\n' 'http_req_duration,1,measure' 'http_req_duration,2,measure' 'http_req_duration,3,measure'
} | gzip >"$fixture/raw/bad__item__vus1.csv.gz"
printf 'stack\tscenario\tvus\tpg_rows\tk6_ok\n' >"$fixture/rowchecks.tsv"

(
  cd "$fixture"
  "$DUCKDB_BIN" -init /dev/null -json < "${BENCH_DIR}/report.sql" > report.json
)
python3 - "$fixture/report.json" <<'PY'
import json
import sys

with open(sys.argv[1]) as report:
    rows = json.load(report)

by_stack = {row["stack"]: row for row in rows}
good = by_stack["quackapi-w1"]
bad = by_stack["fastapi-w1"]

# This guards the manifest-to-raw correlation, raw measure-stage filter, and
# quantile join. A filename parser used to lose the VUS and leave the row with
# no latency samples while this smoke test still passed.
assert good["scenario"] == "item"
assert good["vus"] == 1
assert good["sample_count"] == 3
assert good["p1_ms"] == 1.0
assert good["p50_ms"] == 2.0
assert good["p95_ms"] == 3.0
assert good["p99_ms"] == 3.0
assert good["valid"] is True

assert bad["sample_count"] == 3
assert bad["valid"] is False
assert "measure_check_failures=1" in bad["invalid_reason"]
PY
echo "PASS: report preserves valid and invalid cells with parsed VUs and latency samples"
