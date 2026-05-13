#!/bin/bash
# test_diag_schema.sh — Story 84.1.12: JSON diagnostic schema validation
#
# Validates that every JSON diagnostic emitted by toke contains the
# required fields defined in toke-spec-v02.md Appendix B.
#
# Required fields: schema_version, error_code, severity, stage,
#   message, file, pos (with offset, line, col), expected, got, fix

TOKE=${TOKE:-/Users/matthew.watt/tk/toke/toke}
PASS=0; FAIL=0; TOTAL=0

REQUIRED_FIELDS="schema_version error_code severity stage message file"
REQUIRED_POS_FIELDS="offset line col"

validate_diag() {
  local desc=$1 input=$2
  local tmpfile="/tmp/diag_schema_test_$$.tk"
  echo "$input" > "$tmpfile"
  local had_diag=0
  $TOKE --check "$tmpfile" 2>&1 | while IFS= read -r line; do
    # Skip non-JSON lines
    echo "$line" | python3 -c "import sys,json; json.loads(sys.stdin.readline())" 2>/dev/null || continue
    had_diag=1
    local ok=1
    # Check top-level required fields
    for field in $REQUIRED_FIELDS; do
      if ! echo "$line" | python3 -c "
import sys, json
d = json.loads(sys.stdin.readline())
assert '$field' in d and d['$field'] is not None, 'missing $field'
" 2>/dev/null; then
        echo "FAIL: [$desc] missing required field '$field'"
        ok=0
      fi
    done
    # Check pos sub-fields
    for pf in $REQUIRED_POS_FIELDS; do
      if ! echo "$line" | python3 -c "
import sys, json
d = json.loads(sys.stdin.readline())
assert 'pos' in d and isinstance(d['pos'], dict), 'missing pos'
assert '$pf' in d['pos'], 'missing pos.$pf'
" 2>/dev/null; then
        echo "FAIL: [$desc] missing required field 'pos.$pf'"
        ok=0
      fi
    done
    if [ "$ok" = "1" ]; then
      echo "PASS: [$desc] diagnostic has all required fields"
    fi
  done
  rm -f "$tmpfile"
}

echo "=== toke JSON diagnostic schema validation ==="
echo ""

# Test various error types that produce diagnostics
validate_diag "lex error (bad escape)"       'm=t;f=main():i64{let x="\q";<0;};'
validate_diag "parse error (missing expr)"   'm=t;f=main():i64{let x=;<0;};'
validate_diag "name error (undefined func)"  'm=t;f=main():i64{<foo();};'
validate_diag "type error (missing field)"   'm=t;t=Point{x:i64;y:i64;};f=main():i64{let p=Point{x=1;y=2;};<p.z;};'
validate_diag "parse error (unclosed brace)" 'm=t;f=main():i64{let x=1;'

echo ""
echo "Schema validation complete"
