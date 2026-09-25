#!/usr/bin/env bash
# C088_void_tail_propagation.sh — a tail `!` propagation of a VOID callee in
# a non-void function is E4071 (story 127.188).
#
# THE DEFECT. `f=h(x:i64):i64!$e{ g(x)!$e }` with g a `void!$e` function
# checked clean and built; on g's success path h fell off its end and its
# caller received 0.  127.127 had made an UNRESOLVED tail propagation E4071
# and deliberately excluded one proven void.  The shape is the same, and
# E4071's propagation message already says it.
#
# WHAT IS ASSERTED.
#   1  the void tail propagation: E4071, got "void", no fix;
#   2  the same function with `<0` after the propagation checks clean and
#      returns 0 via the ok path (the err path is not asserted: a `void!$e`
#      callee's error does not reach the caller's `!` — a separate codegen
#      defect, reported with this story);
#   3  a void tail propagation in a `void!$e` function is complete, not a
#      missing return.
#
# Negative control: TKC built from 84e1f4d — case 1 checks clean
# (3 of 6 red: the three case-1 assertions).
#
# Story: 127.188

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
errs() { "${TKC}" --check "$1" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
runs() { "${TKC}" "$1" -o "${WORK}/b" >/dev/null 2>&1 && "${WORK}/b" 2>/dev/null | tr '\n' ' '; }
field() { "${TKC}" --check "$1" 2>&1 | awk -v k="$2" '/"error_code":"E4071"/{ if (match($0,"\""k"\":\"[^\"]*\"")) print substr($0,RSTART+length(k)+4,RLENGTH-length(k)-5); else print "absent" }'; }

HDR='m=t;
i=io:std.io;
t=$e{$bad:i64};
f=g(x:i64):void!$e{ if(x<0){<$e{$bad:x}}; io.println("g") };'

printf '%s\nf=h(x:i64):i64!$e{ g(x)!$e };\nf=main():i64{ let r=mt h(1) {$ok:v v;$err:e 7}; io.println("\\(r)"); <0 };\n' "$HDR" > "${WORK}/p1.tk"
expect "1-void-tail-prop-e4071" "$(errs "${WORK}/p1.tk")" "E4071 "
expect "1-got-void"             "$(field "${WORK}/p1.tk" got)" "void"
expect "1-no-fix"               "$(field "${WORK}/p1.tk" fix)" "absent"

printf '%s\nf=h(x:i64):i64!$e{ g(x)!$e; <0 };\nf=main():i64{ let a=mt h(1) {$ok:v v;$err:e 7}; io.println("\\(a)"); <0 };\n' "$HDR" > "${WORK}/p2.tk"
expect "2-with-return-checks" "$(errs "${WORK}/p2.tk")" ""
expect "2-with-return-runs"   "$(runs "${WORK}/p2.tk")" "g 0 "

printf '%s\nf=k(x:i64):void!$e{ g(x)!$e };\nf=main():i64{ mt k(1) {$ok:v io.println("ok");$err:e io.println("err")}; <0 };\n' "$HDR" > "${WORK}/p3.tk"
expect "3-void-function-fine" "$(errs "${WORK}/p3.tk")" ""

echo "C088: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
