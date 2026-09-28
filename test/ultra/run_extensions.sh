#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${EXTENSION_RESULTS_DIR:-${ROOT}/test/ultra/results/extensions-$(date -u +%Y%m%dT%H%M%SZ)}"
mkdir -p "$OUT"

strip_ansi() {
  sed $'s/\\033\\[[0-9;]*m//g'
}

declare -a candidates=()
if [[ -n "${DUCKDB:-}" ]]; then
  candidates+=("$DUCKDB")
fi
candidates+=("${ROOT}/build/release/duckdb")
candidates+=("${ROOT}/build/debug/duckdb")
system_duckdb="$(command -v duckdb 2>/dev/null || true)"
[[ -n "$system_duckdb" ]] && candidates+=("$system_duckdb")

chosen=""
ext_dir=""
for binary in "${candidates[@]}"; do
  [[ -x "$binary" ]] || continue
  version="$($binary -init /dev/null --version 2>/dev/null | strip_ansi | awk '$1 ~ /^v[0-9]/ {print $1; exit}')"
  [[ -n "$version" ]] || continue
  for platform in osx_arm64 osx_amd64 linux_arm64 linux_amd64; do
    candidate_dir="${HOME}/.duckdb/extensions/${version}/${platform}"
    if [[ -f "${candidate_dir}/httpfs_timeout_retry.duckdb_extension" && -f "${candidate_dir}/cache_prewarm.duckdb_extension" ]]; then
      chosen="$binary"
      ext_dir="$candidate_dir"
      break 2
    fi
  done
done

DUCKDB_BIN="${chosen:-${DUCKDB:-${ROOT}/build/release/duckdb}}"
printf 'duckdb\t%s\n' "$DUCKDB_BIN" >"${OUT}/manifest.tsv"
if [[ -z "$chosen" ]]; then
  printf 'status\tSKIP\nreason\tno version-matched httpfs_timeout_retry + cache_prewarm pair\n' >>"${OUT}/manifest.tsv"
  echo "SKIP: no version-matched optional extension set; wrote ${OUT}/manifest.tsv"
  exit 0
fi

printf 'extension_dir\t%s\nstatus\tRUN\n' "$ext_dir" >>"${OUT}/manifest.tsv"

sql="${OUT}/probe.sql"
{
  printf "LOAD '%s/httpfs_timeout_retry.duckdb_extension';\n" "$ext_dir"
  printf "LOAD '%s/cache_prewarm.duckdb_extension';\n" "$ext_dir"
  for optional in cache_httpfs http_stats finetype query_condition_cache table_guard; do
    if [[ -f "${ext_dir}/${optional}.duckdb_extension" ]]; then
      printf "LOAD '%s/%s.duckdb_extension';\n" "$ext_dir" "$optional"
    fi
  done
  cat <<'SQL'
CREATE TABLE extension_prewarm_probe AS SELECT range AS id FROM range(100);
SELECT extension_name, loaded
FROM duckdb_extensions()
WHERE extension_name IN ('httpfs_timeout_retry','cache_prewarm','cache_httpfs','http_stats','finetype','query_condition_cache','table_guard')
ORDER BY extension_name;
SET httpfs_timeout_file_operation_ms = 1000;
SET httpfs_retries_file_operation = 2;
SELECT name, value
FROM duckdb_settings()
WHERE name IN ('httpfs_timeout_file_operation_ms','httpfs_retries_file_operation','cache_httpfs_type','cache_httpfs_max_in_mem_cache_block_count')
ORDER BY name;
SELECT function_name, function_type, array_to_string(parameter_types, ',') AS parameter_types
FROM duckdb_functions()
WHERE function_name IN ('prewarm','prewarm_remote','finetype','finetype_detail','finetype_validate','condition_cache_info','condition_cache_stats','table_guard_status')
ORDER BY function_name, parameter_types;
SQL
  if [[ -f "${ext_dir}/finetype.duckdb_extension" ]]; then
    printf "SELECT finetype_version() AS finetype_version, finetype('42') AS inferred_type, finetype_detail('42') AS detail;\n"
  fi
  if [[ -f "${ext_dir}/cache_prewarm.duckdb_extension" ]]; then
    printf "SELECT try(prewarm('extension_prewarm_probe','read')) AS prewarm_local_probe;\n"
  fi
} >"$sql"

if [[ "$DUCKDB_BIN" == *"/build/debug/duckdb" ]]; then
  ASAN_OPTIONS="${ASAN_OPTIONS:-detect_container_overflow=0}" "$DUCKDB_BIN" -init /dev/null -unsigned -json <"$sql" >"${OUT}/probe.json"
else
  "$DUCKDB_BIN" -init /dev/null -unsigned -json <"$sql" >"${OUT}/probe.json"
fi
printf 'status\tPASS\n' >>"${OUT}/manifest.tsv"
echo "PASS: optional extension probes written to ${OUT}"
