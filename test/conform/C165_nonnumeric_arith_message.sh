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
# Cases 6-10 (127.223 items 3 and 4): the E1003 leading-underscore and the
# two E1004 digit-then-letter paths carried guidance fixes ("remove
# underscores: ...", "identifiers must start with a-z or A-Z") although
# docs/spec/errors.md specifies the field absent and dropping an underscore
# can collide with another name; W1001 reported stage "lex" (its number is in
# the 1xxx range, which it shares with the lexer's E1001) and carried "ensure
# the value fits ...", also specified absent.  Case 10 guards E1001.
#
# Negative control: TKC built from main at a705c93 — cases 1-3 and 6-9 red.
#
# Story: 127.224, 127.223

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

# 127.223 items 3 and 4: guidance `fix` text dropped where errors.md
# specifies the field absent, and W1001 reports the stage that raises it.
lexdiag() {
    printf '%s\n' "$1" > "${WORK}/l.tk"
    "${TKC}" --check "${WORK}/l.tk" 2>&1 | head -1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){c=substr($0,RSTART+14,RLENGTH-15); match($0,/"stage":"[a-z_]+"/); st=substr($0,RSTART+9,RLENGTH-10); f=index($0,"\"fix\":")?"fix":"nofix"; printf "%s|%s|%s",c,st,f}'
}
expect "6-E1003-leading-underscore-nofix" "$(lexdiag 'm=t;f=main():i64{let _x=1;<0};')"   "E1003|lex|nofix"
expect "7-E1004-hex-letter-nofix"         "$(lexdiag 'm=t;f=main():i64{let a=0x1g;<0};')" "E1004|lex|nofix"
expect "8-E1004-bin-letter-nofix"         "$(lexdiag 'm=t;f=main():i64{let a=0b1z;<0};')" "E1004|lex|nofix"
expect "9-W1001-stage-type-check-nofix"   "$(lexdiag 'm=t;f=main():i64{let a:f64=1.5;let b=a as i64;<b};')" "W1001|type_check|nofix"
expect "10-E1001-stage-still-lex"         "$(lexdiag 'm=t;f=main():str{<"\q"};')" "E1001|lex|nofix"

echo "C165: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
