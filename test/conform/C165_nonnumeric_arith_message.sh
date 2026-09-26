#!/usr/bin/env bash
# C165_nonnumeric_arith_message.sh — arithmetic on a non-numeric operand says
# what the operator requires, and carries no `fix` (story 127.224 item 5).
#
# THE DEFECT.  `true+false` reached emit_mm, which formats the two OPERAND
# types: "type mismatch: expected 'bool', got 'bool'", with fix "cast RHS to
# bool using 'as'" — a cast that does not make `+` legal on a bool, so the
# fix was wrong in every case it was emitted (AGENTS.md §3.1).  Same for a
# struct (`q-q`) and a bool on one side (`b*2`: "cast RHS to bool").  The
# message now names the operator and the non-numeric side, expected "a
# numeric type", and the fix field is absent.
#
# Negative control: TKC built from main at a705c93 — cases 1-3 red.
#
# Story: 127.224

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|message|fix-present for each diagnostic
diag() {
    printf 'm=t;\nt=$p{x:i64};\nf=main():i64{\n  let b:bool=true;\n  let q=$p{x:1};\n  let z=%s;\n  <0\n};\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){c=substr($0,RSTART+14,RLENGTH-15); match($0,/"message":"[^"]*"/); m=substr($0,RSTART+11,RLENGTH-12); f=index($0,"\"fix\":")?"fix":"nofix"; printf "%s|%s|%s ",c,m,f}'
}
expect "1-bool-plus-bool" "$(diag 'true+false')" "E4031|'+' requires numeric operands: both sides are 'bool'|nofix "
expect "2-bool-times-int" "$(diag 'b*2')"        "E4031|'*' requires numeric operands: the left side is 'bool'|nofix "
expect "3-struct-minus"   "$(diag 'q-q')"        "E4031|'-' requires numeric operands: both sides are 'p'|nofix "
# guards: the numeric-mismatch and str+str diagnostics are unchanged
expect "4-int-float-unchanged" "$(diag '1+2.0')" "E4031|type mismatch: expected 'i64', got 'f64'|fix "
expect "5-numeric-ok" "$(diag '1+2')" ""

echo "C165: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
