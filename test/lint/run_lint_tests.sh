#!/bin/bash
# Lint rule test runner
TKC="${1:-./toke}"
PASS=0; FAIL=0

check_warns() {
  local file="$1" rule="$2"
  local output=$($TKC --lint "$file" 2>&1)
  if echo "$output" | grep -q "$rule"; then
    PASS=$((PASS+1)); echo "  PASS $file (warns $rule)"
  else
    FAIL=$((FAIL+1)); echo "  FAIL $file (expected $rule warning)"
  fi
}

check_clean() {
  local file="$1"
  local output=$($TKC --lint "$file" 2>&1)
  if ! echo "$output" | grep -q '"severity":"warning"'; then
    PASS=$((PASS+1)); echo "  PASS $file (clean)"
  else
    FAIL=$((FAIL+1)); echo "  FAIL $file (unexpected warnings)"
  fi
}

echo "=== Lint rule tests ==="
check_warns test/lint/unreachable_positive.tk "unreachable"
check_clean test/lint/unreachable_negative.tk
check_warns test/lint/emptyfn_positive.tk "empty"
check_clean test/lint/emptyfn_negative.tk
check_warns test/lint/unusedimport_positive.tk "unused"
check_clean test/lint/unusedimport_negative.tk
check_warns test/lint/redundantbind_positive.tk "redundant"
check_clean test/lint/redundantbind_negative.tk

echo "---"
echo "$PASS passed, $FAIL failed"
exit $FAIL
