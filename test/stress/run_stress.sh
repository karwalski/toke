#!/usr/bin/env bash
# Stress tests for tkc: boundary integers, large arrays, deep recursion.
# Each .tk file has a companion .expected file with the expected exit code.
set -uo pipefail
PASS=0 FAIL=0 SKIP=0
TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
DIR="$(cd "$(dirname "$0")" && pwd)"
TMPDIR="${TMPDIR:-/tmp}"

for tk in "${DIR}"/*.tk; do
  [ -f "$tk" ] || continue
  name=$(basename "$tk" .tk)
  exp_file="${DIR}/${name}.expected"
  if [ ! -f "$exp_file" ]; then
    echo "SKIP $name (no .expected file)"
    SKIP=$((SKIP+1))
    continue
  fi
  expected=$(tr -d ' \r\n' < "$exp_file")

  # Compile
  bin="${TMPDIR}/tkc_stress_${name}"
  if ! "$TKC" --out "$bin" "$tk" 2>/dev/null; then
    echo "FAIL $name (compile failed)"
    FAIL=$((FAIL+1))
    continue
  fi

  # Run with 10-second timeout (use gtimeout on macOS if available)
  TIMEOUT_CMD=""
  if command -v timeout >/dev/null 2>&1; then
    TIMEOUT_CMD="timeout 10"
  elif command -v gtimeout >/dev/null 2>&1; then
    TIMEOUT_CMD="gtimeout 10"
  fi
  $TIMEOUT_CMD "$bin" 2>/dev/null; actual=$?
  rm -f "$bin"

  if [ "$actual" = "$expected" ]; then
    echo "PASS $name"
    PASS=$((PASS+1))
  else
    echo "FAIL $name (got $actual, expected $expected)"
    FAIL=$((FAIL+1))
  fi
done

echo "---"
echo "$PASS passed, $FAIL failed, $SKIP skipped"
[ $FAIL -eq 0 ]
