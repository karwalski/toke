#!/usr/bin/env bash
# End-to-end test runner for tkc.
# Each .tk file has a companion .expected file containing the expected exit code.
set -uo pipefail
PASS=0 FAIL=0
TKC="$(cd "$(dirname "$0")/../.." && pwd)/tkc"
DIR="$(cd "$(dirname "$0")" && pwd)"
for tk in "${DIR}"/*.tk; do
  name=$(basename "$tk" .tk)
  exp_file="${DIR}/${name}.expected"
  if [ ! -f "$exp_file" ]; then
    echo "SKIP $name (no .expected file)"
    continue
  fi
  expected=$(tr -d ' \r\n' < "$exp_file")
  if ! "$TKC" --out "/tmp/tkc_e2e_${name}" "$tk" 2>/tmp/tkc_e2e_err_${name}; then
    echo "FAIL $name (tkc compile failed)"
    cat /tmp/tkc_e2e_err_${name}
    FAIL=$((FAIL+1))
    rm -f /tmp/tkc_e2e_err_${name}
    continue
  fi
  rm -f /tmp/tkc_e2e_err_${name}
  "/tmp/tkc_e2e_${name}" 2>/dev/null; actual=$?
  rm -f "/tmp/tkc_e2e_${name}"
  if [ "$actual" = "$expected" ]; then echo "PASS $name"; PASS=$((PASS+1))
  else echo "FAIL $name (got $actual, expected $expected)"; FAIL=$((FAIL+1)); fi
done
echo "---"; echo "$PASS passed, $FAIL failed"
[ $FAIL -eq 0 ]
