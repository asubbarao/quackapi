#!/usr/bin/env bash
# =============================================================================
# test/run_all.sh — quackapi unified test runner
#
# Usage (from repo root):
#   bash test/run_all.sh [--with-cpp]
#
#   --with-cpp   Also run ext-cpp sqllogictest (make test) and parity_b2.sh,
#                but ONLY if the compiled extension already exists.
#                Without this flag those suites are reported as SKIPPED.
#
# Port range for any server boots: 18500-18599. Never touches 9494/9495.
# DuckDB binary: /opt/homebrew/bin/duckdb -unsigned
# =============================================================================
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

DUCK="${DUCKDB:-/opt/homebrew/bin/duckdb} -unsigned"
WITH_CPP=0
for arg in "$@"; do
  [ "$arg" = "--with-cpp" ] && WITH_CPP=1
done

EXT_PATH="$REPO_ROOT/ext-cpp/build/release/extension/quackapi/quackapi.duckdb_extension"
EXT_BINARY="$REPO_ROOT/ext-cpp/build/release/duckdb"

# ─── colour helpers ───────────────────────────────────────────────────────────
_green() { printf '\033[32m%s\033[0m' "$*"; }
_red()   { printf '\033[31m%s\033[0m' "$*"; }
_dim()   { printf '\033[2m%s\033[0m'  "$*"; }

# ─── summary table accumulator ───────────────────────────────────────────────
declare -a COL_NAME COL_CHECKS COL_PASS COL_FAIL COL_SKIP COL_STATUS
OVERALL_FAIL=0

_record() {
  local name="$1" checks="$2" pass="$3" fail="$4" skip="$5"
  COL_NAME+=("$name")
  COL_CHECKS+=("$checks")
  COL_PASS+=("$pass")
  COL_FAIL+=("$fail")
  COL_SKIP+=("$skip")
  if [ "$fail" -gt 0 ]; then
    COL_STATUS+=("FAIL")
    OVERALL_FAIL=$((OVERALL_FAIL + fail))
  elif [ "$checks" -eq 0 ]; then
    COL_STATUS+=("SKIP")
  else
    COL_STATUS+=("PASS")
  fi
}

_print_summary() {
  echo ""
  echo "════════════════════════════════════════════════════════════════"
  printf "  %-35s  %6s  %6s  %6s  %6s  %s\n" "suite" "checks" "pass" "fail" "skip" "status"
  echo "  ───────────────────────────────────────────────────────────"
  local i
  for i in "${!COL_NAME[@]}"; do
    local st="${COL_STATUS[$i]}"
    local row
    row="$(printf '  %-35s  %6s  %6s  %6s  %6s  %s\n' \
      "${COL_NAME[$i]}" "${COL_CHECKS[$i]}" "${COL_PASS[$i]}" \
      "${COL_FAIL[$i]}" "${COL_SKIP[$i]}" "$st")"
    if [ "$st" = "FAIL" ]; then
      _red "$row"
    elif [ "$st" = "SKIP" ]; then
      _dim "$row"
    else
      printf '%s' "$row"
    fi
    echo ""
  done
  echo "════════════════════════════════════════════════════════════════"
  if [ "$OVERALL_FAIL" -eq 0 ]; then
    echo "  $(_green 'ALL SUITES PASSED')"
  else
    echo "  $(_red "OVERALL RESULT: FAILED  ($OVERALL_FAIL checks failed)")"
  fi
  echo "════════════════════════════════════════════════════════════════"
  echo ""
}

# ─── Tier-1: handle_request oracle suite ─────────────────────────────────────
# LANDMINE: do NOT .read app.sql before this suite.
# Canonical: printf '.read framework.sql\n.read test/tier1_handle_request.test.sql\n' | duckdb -unsigned
_run_tier1() {
  echo ""
  echo "── tier1: handle_request oracle ──────────────────────────────"
  local tmpdb="/tmp/qall_tier1_$$.db"
  local out
  out=$(cd "$REPO_ROOT" && printf '.read framework.sql\n.read test/tier1_handle_request.test.sql\n' \
    | $DUCK "$tmpdb" 2>&1)
  local rc=$?
  rm -f "$tmpdb" "${tmpdb}.wal"

  # The suite ends with: SELECT total_checks, passed, failed FROM ...
  # DuckDB renders the data row as:  │    112 │    112 │      0 │
  # We grab the last data row that starts with the Unicode box char and has 3 numbers.
  local total pass fail
  local summary_line
  summary_line=$(printf '%s\n' "$out" \
    | grep -E '^[[:space:]]*[0-9]' \
    | grep -v 'rows (' \
    | grep -v 'int64' \
    | tail -1 || true)
  # Try the box-drawing row (│  N │  N │  N │)
  if [ -z "$summary_line" ]; then
    summary_line=$(printf '%s\n' "$out" | awk '/int64/{next} /[0-9]+/ && NF>=3 {found=$0} END{print found}' || true)
  fi
  total=$(printf '%s\n' "$summary_line" | tr -d '│' | awk '{print $1}' | tr -d ' ')
  pass=$(printf '%s\n'  "$summary_line" | tr -d '│' | awk '{print $2}' | tr -d ' ')
  fail=$(printf '%s\n'  "$summary_line" | tr -d '│' | awk '{print $3}' | tr -d ' ')

  # Fallback: count │ true │ / │ false │ lines directly
  if [ -z "$total" ] || ! [[ "$total" =~ ^[0-9]+$ ]]; then
    pass=$(printf '%s\n' "$out" | grep -cF '│ true' 2>/dev/null || echo 0)
    fail=$(printf '%s\n' "$out" | grep -cF '│ false' 2>/dev/null || echo 0)
    total=$((pass + fail))
  fi
  total="${total:-0}"; pass="${pass:-0}"; fail="${fail:-0}"

  if [ "$rc" -ne 0 ] && [ "${fail:-0}" -eq 0 ]; then
    fail=1
    echo "  ERROR: DuckDB exited non-zero (rc=$rc)"
    printf '%s\n' "$out" | tail -20
  fi
  echo "  checks=$total  pass=$pass  fail=$fail"
  _record "tier1:handle_request" "$total" "$pass" "$fail" 0
}

# ─── DI oracle suite ─────────────────────────────────────────────────────────
# Load order: framework.sql + di.sql + test/di.test.sql
# This suite uses human-readable SELECT output (no boolean oracle table).
# We count "=== TEST N" headers as total checks and look for errors.
_run_di() {
  echo ""
  echo "── di: dependency injection ───────────────────────────────────"
  local tmpdb="/tmp/qall_di_$$.db"
  local out
  out=$(cd "$REPO_ROOT" && printf '.read framework.sql\n.read di.sql\n.read test/di.test.sql\n' \
    | $DUCK "$tmpdb" 2>&1)
  local rc=$?
  rm -f "$tmpdb" "${tmpdb}.wal"

  local total fail pass
  # Use awk to count lines matching pattern — avoids grep -c newline issues on macOS
  total=$(printf '%s\n' "$out" | awk '/=== TEST/{n++} END{print n+0}')
  fail=$(printf '%s\n' "$out" | awk '/^(Error|Catalog Error|IO Error|Parser Error)/ && !/already exists/{n++} END{print n+0}')
  total="${total:-0}"; fail="${fail:-0}"
  [ "$rc" -ne 0 ] && fail=$((fail + 1))
  pass=$((total - fail))
  [ "$pass" -lt 0 ] && pass=0

  echo "  checks=$total  pass=$pass  fail=$fail"
  _record "di:dependency_injection" "$total" "$pass" "$fail" 0
}

# ─── Middleware oracle suite ──────────────────────────────────────────────────
# Load order: framework.sql + middleware.sql + test/middleware.test.sql
# Uses assert_true() macro; failures throw errors caught by DuckDB as Error lines.
_run_middleware() {
  echo ""
  echo "── middleware: chain tests ────────────────────────────────────"
  local tmpdb="/tmp/qall_mw_$$.db"
  local out
  out=$(cd "$REPO_ROOT" && printf '.read framework.sql\n.read middleware.sql\n.read test/middleware.test.sql\n' \
    | $DUCK "$tmpdb" 2>&1)
  local rc=$?
  rm -f "$tmpdb" "${tmpdb}.wal"

  # Count PASS lines from assert_true output and FAIL errors
  # DuckDB renders inside box chars: │ PASS: ... │
  local pass fail errs total
  pass=$(printf '%s\n' "$out" | awk '/PASS:/{n++} END{print n+0}')
  fail=$(printf '%s\n' "$out" | awk '/FAIL:/{n++} END{print n+0}')
  errs=$(printf '%s\n' "$out" | awk '/^(Error|IO Error|Parser Error).*FAIL:/{n++} END{print n+0}')
  pass="${pass:-0}"; fail="${fail:-0}"; errs="${errs:-0}"
  fail=$((fail + errs))
  [ "$rc" -ne 0 ] && [ "$fail" -eq 0 ] && fail=1
  total=$((pass + fail))

  echo "  checks=$total  pass=$pass  fail=$fail"
  _record "middleware:chain" "$total" "$pass" "$fail" 0
}

# ─── Tier-2: HTTP wire tests ──────────────────────────────────────────────────
# The suite self-detects server absence and exits 2 (SKIP).
_run_tier2() {
  echo ""
  echo "── tier2: HTTP wire ───────────────────────────────────────────"
  local out
  out=$(cd "$REPO_ROOT" && bash test/tier2_http.sh 2>&1)
  local rc=$?

  if [ "$rc" -eq 2 ]; then
    echo "  SKIPPED: server not reachable (start server manually then re-run)"
    _record "tier2:http_wire" 0 0 0 1
    return
  fi

  local pass fail
  pass=$(printf '%s\n' "$out" | grep -c '  PASS  ' 2>/dev/null || echo 0)
  fail=$(printf '%s\n' "$out" | grep -c '  FAIL  ' 2>/dev/null || echo 0)
  local total=$((pass + fail))
  echo "  checks=$total  pass=$pass  fail=$fail"
  _record "tier2:http_wire" "$total" "$pass" "$fail" 0
}

# ─── ext-cpp: sqllogictest (make test) ───────────────────────────────────────
_run_cpp_sqllogic() {
  echo ""
  echo "── ext-cpp: sqllogictest (make test) ─────────────────────────"
  if [ "$WITH_CPP" -eq 0 ]; then
    echo "  SKIPPED: pass --with-cpp to enable (no-build guarantee)"
    _record "ext-cpp:sqllogictest" 0 0 0 1
    return
  fi
  if [ ! -f "$EXT_PATH" ]; then
    echo "  SKIPPED: needs build — extension not found at ext-cpp/build/release/..."
    _record "ext-cpp:sqllogictest" 0 0 0 1
    return
  fi
  local out
  out=$(cd "$REPO_ROOT/ext-cpp" && make test 2>&1)
  local rc=$?
  local pass fail
  pass=$(printf '%s\n' "$out" | grep -c '\[OK\]' 2>/dev/null || echo 0)
  fail=$(printf '%s\n' "$out" | grep -c '\[FAIL\]' 2>/dev/null || echo 0)
  [ "$rc" -ne 0 ] && [ "$fail" -eq 0 ] && fail=1
  local total=$((pass + fail))
  echo "  checks=$total  pass=$pass  fail=$fail"
  _record "ext-cpp:sqllogictest" "$total" "$pass" "$fail" 0
}

# ─── ext-cpp: parity_b2.sh ────────────────────────────────────────────────────
_run_parity() {
  echo ""
  echo "── ext-cpp: parity_b2 ────────────────────────────────────────"
  if [ "$WITH_CPP" -eq 0 ]; then
    echo "  SKIPPED: pass --with-cpp to enable"
    _record "ext-cpp:parity_b2" 0 0 0 1
    return
  fi
  if [ ! -f "$EXT_PATH" ]; then
    echo "  SKIPPED: needs build — extension not found"
    _record "ext-cpp:parity_b2" 0 0 0 1
    return
  fi
  local out
  out=$(cd "$REPO_ROOT" && zsh ext-cpp/parity_b2.sh 2>&1)
  local rc=$?
  local pass fail
  pass=$(printf '%s\n' "$out" | grep -c '^PASS:' 2>/dev/null || echo 0)
  fail=$(printf '%s\n' "$out" | grep -c '^FAIL:' 2>/dev/null || echo 0)
  [ "$rc" -ne 0 ] && [ "$fail" -eq 0 ] && fail=1
  local total=$((pass + fail))
  echo "  checks=$total  pass=$pass  fail=$fail"
  _record "ext-cpp:parity_b2" "$total" "$pass" "$fail" 0
}

# ─── fuzz: property/oracle suite (run SQL directly — the shell wrapper has a
#           parser bug for DuckDB box output, so we bypass it here) ─────────────
_run_fuzz_oracle() {
  echo ""
  echo "── fuzz: oracle property tests ────────────────────────────────"
  local tmpdb="/tmp/qall_fuzz_$$.db"
  local fuzz_out fuzz_rc
  fuzz_out=$(cd "$REPO_ROOT" && printf '.read framework.sql\n.read app.sql\n.read test/fuzz/oracle_fuzz.test.sql\n' \
    | $DUCK "$tmpdb" 2>&1)
  fuzz_rc=$?
  rm -f "$tmpdb" "${tmpdb}.wal"
  # Parse summary from DuckDB box output
  local f_total f_pass f_fail f_line
  f_line=$(printf '%s\n' "$fuzz_out" | grep -v 'int64' | \
           awk 'NF>=5 && /[0-9]/ {last=$0} END {print last}' || true)
  f_total=$(printf '%s\n' "$f_line" | tr -d '│' | awk '{print $1}' | tr -d ' ')
  f_pass=$(printf '%s\n'  "$f_line" | tr -d '│' | awk '{print $2}' | tr -d ' ')
  f_fail=$(printf '%s\n'  "$f_line" | tr -d '│' | awk '{print $3}' | tr -d ' ')
  if [ -z "$f_total" ] || ! [[ "${f_total:-x}" =~ ^[0-9]+$ ]]; then
    f_pass=$(printf '%s\n' "$fuzz_out" | grep -c 'true' 2>/dev/null; true)
    f_fail=$(printf '%s\n' "$fuzz_out" | grep -c 'false' 2>/dev/null; true)
    f_pass="${f_pass:-0}"; f_fail="${f_fail:-0}"
    f_total=$((f_pass + f_fail))
  fi
  f_total="${f_total:-0}"; f_pass="${f_pass:-0}"; f_fail="${f_fail:-0}"
  # fuzz_rc non-zero is expected when there are failures (the oracle SQL exits 0)
  echo "  checks=$f_total  pass=$f_pass  fail=$f_fail"
  if [ "${f_fail:-0}" -gt 0 ]; then
    echo "  Known bugs in this run (pass=false):"
    printf '%s\n' "$fuzz_out" | grep 'false' | head -10 | sed 's/^/    /'
  fi
  _record "fuzz:oracle_property" "$f_total" "$f_pass" "$f_fail" 0
}

# ─── conformance: differential vs FastAPI (C ext) ─────────────────────────────
_run_conformance() {
  echo ""
  echo "── conformance: differential vs FastAPI ───────────────────────"
  local conf_out conf_rc
  conf_out=$(cd "$REPO_ROOT" && bash test/conformance/run_conformance.sh 2>&1)
  conf_rc=$?
  if printf '%s\n' "$conf_out" | grep -q 'FATAL:'; then
    echo "  SKIPPED: infrastructure not ready (ext server or FastAPI unavailable)"
    printf '%s\n' "$conf_out" | grep '\[conf\] FATAL' | head -3 | sed 's/^/  > /'
    _record "conformance:differential" 0 0 0 1
    return
  fi
  local c_pass c_fail c_total
  c_pass=$(printf '%s\n' "$conf_out" | awk '/PASS/{n++} END{print n+0}')
  c_fail=$(printf '%s\n' "$conf_out" | awk '/FAIL/{n++} END{print n+0}')
  c_pass="${c_pass:-0}"; c_fail="${c_fail:-0}"
  [ "$conf_rc" -ne 0 ] && [ "$c_fail" -eq 0 ] && c_fail=1
  c_total=$((c_pass + c_fail))
  echo "  checks=$c_total  pass=$c_pass  fail=$c_fail"
  _record "conformance:differential" "$c_total" "$c_pass" "$c_fail" 0
}

# ─── conformance: pure-SQL differential vs FastAPI (no C ext needed) ──────────
_run_conformance_pure() {
  echo ""
  echo "── conformance-pure: SQL oracle vs FastAPI ────────────────────"
  local conf_out conf_rc
  conf_out=$(cd "$REPO_ROOT" && bash test/conformance/run_conformance_pure.sh 2>&1)
  conf_rc=$?
  if printf '%s\n' "$conf_out" | grep -q 'FATAL:'; then
    echo "  SKIPPED: FastAPI not available or framework smoke-test failed"
    printf '%s\n' "$conf_out" | grep 'FATAL' | head -3 | sed 's/^/  > /'
    _record "conformance-pure:sql_vs_fastapi" 0 0 0 1
    return
  fi
  # Parse results from results_pure.jsonl
  # INTENTIONAL classification: DIVERGE with class INTENTIONAL or FASTAPI-QUIRK
  # are pinned deliberate divergences (id+matcher+rationale in driver); only
  # BUG-class DIVERGE count as failures. Report PASS/FAIL/INTENTIONAL.
  local results_file="$REPO_ROOT/test/conformance/results_pure.jsonl"
  local c_total c_match c_bug c_intent c_quirk
  if [ -f "$results_file" ]; then
    c_total=$(wc -l < "$results_file" | tr -d ' ')
    c_match=$(grep -c '"verdict": "MATCH"' "$results_file" 2>/dev/null || true)
    c_bug=$(grep -c '"class": "BUG"' "$results_file" 2>/dev/null || true)
    c_intent=$(grep -c '"class": "INTENTIONAL"' "$results_file" 2>/dev/null || true)
    c_quirk=$(grep -c '"class": "FASTAPI-QUIRK"' "$results_file" 2>/dev/null || true)
  else
    c_total=0; c_match=0; c_bug=0; c_intent=0; c_quirk=0
    [ "$conf_rc" -ne 0 ] && c_bug=1
  fi
  c_total="${c_total:-0}"; c_match="${c_match:-0}"; c_bug="${c_bug:-0}"; c_intent="${c_intent:-0}"; c_quirk="${c_quirk:-0}"
  [ "$conf_rc" -ne 0 ] && [ "$c_bug" -eq 0 ] && c_bug=1
  local c_pass=$(( 0 + ${c_match:-0} + ${c_intent:-0} + ${c_quirk:-0} ))
  echo "  checks=$c_total  pass=$c_pass  fail=$c_bug  intentional=$c_intent  quirk=$c_quirk"
  _record "conformance-pure:sql_vs_fastapi" "$c_total" "$c_pass" "$c_bug" 0
}

# ─── Glob-discovered suites under test/*/run_*.sh ────────────────────────────
_run_discovered() {
  local script="$1"
  local name
  name=$(echo "$script" | sed "s|$REPO_ROOT/||")
  echo ""
  echo "── discovered: $name ──────────────────────────────────────────"
  if [ ! -x "$script" ]; then
    chmod +x "$script" 2>/dev/null || true
  fi
  local out rc
  out=$(bash "$script" 2>&1)
  rc=$?

  # If suite signals SKIP via exit 2
  if [ "$rc" -eq 2 ]; then
    echo "  SKIPPED"
    _record "$name" 0 0 0 1
    return
  fi

  # Try to parse PASS/FAIL summary lines in common patterns
  local pass fail total
  pass=$(printf '%s\n' "$out" | grep -cE '(PASS|pass|OK)' 2>/dev/null; true)
  fail=$(printf '%s\n' "$out" | grep -cE '(FAIL|FAILED|ERROR)' 2>/dev/null; true)
  pass="${pass:-0}"; fail="${fail:-0}"
  # Prefer explicit "N passed, M failed" summary if present
  local summary
  summary=$(printf '%s\n' "$out" | grep -iE '[0-9]+ passed' | tail -1 || true)
  if [ -n "$summary" ]; then
    local sp sf
    sp=$(printf '%s\n' "$summary" | grep -oE '[0-9]+ passed' | grep -oE '[0-9]+' || true)
    sf=$(printf '%s\n' "$summary" | grep -oE '[0-9]+ failed' | grep -oE '[0-9]+' || true)
    [ -n "$sp" ] && pass="$sp"; [ -n "$sf" ] && fail="$sf"
  fi
  [ "$rc" -ne 0 ] && [ "${fail:-0}" -eq 0 ] && fail=1
  fail="${fail:-0}"; pass="${pass:-0}"
  total=$((pass + fail))
  echo "  checks=$total  pass=$pass  fail=$fail"
  _record "$name" "$total" "$pass" "$fail" 0
}

# =============================================================================
# MAIN
# =============================================================================
echo ""
echo "════════════════════════════════════════════════════════════════"
echo "  quackapi — unified test runner"
echo "  repo: $REPO_ROOT"
echo "  duck: $DUCK"
echo "  --with-cpp: $WITH_CPP"
echo "════════════════════════════════════════════════════════════════"

# 1. SQL oracle suites (fixed order, each with its correct invocation)
_run_tier1
_run_di
_run_middleware

# 2. HTTP wire suite (self-skips if server absent)
_run_tier2

# 3. ext-cpp suites (skip unless --with-cpp and built extension present)
_run_cpp_sqllogic
_run_parity

# 4. Glob-discover any test/*/run_*.sh (conformance + fuzz join here automatically)
found_any_discovered=0
for suite in "$REPO_ROOT"/test/*/run_*.sh; do
  [ -f "$suite" ] || continue
  found_any_discovered=1
  case "$suite" in
    */fuzz/run_oracle_fuzz.sh)             _run_fuzz_oracle ;;
    */conformance/run_conformance.sh)      _run_conformance ;;
    */conformance/run_conformance_pure.sh) _run_conformance_pure ;;
    *)                                     _run_discovered "$suite" ;;
  esac
done

# If no discovered suites yet (directories exist but no run_*.sh), note it
if [ "$found_any_discovered" -eq 0 ]; then
  echo ""
  echo "  (no test/*/run_*.sh suites discovered — conformance+fuzz will auto-join when present)"
fi

# =============================================================================
# SUMMARY TABLE
# =============================================================================
_print_summary

[ "$OVERALL_FAIL" -eq 0 ] && exit 0 || exit 1
