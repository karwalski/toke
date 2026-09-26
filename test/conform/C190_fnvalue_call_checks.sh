#!/usr/bin/env bash
# C190_fnvalue_call_checks.sh — a call through a function VALUE is checked
# against that value's signature (story 124.0c item 2).
#
# THE DEFECT, on main at 7c16729: types.c checked arguments only for a call
# that resolves to a module f= declaration.  A call through a closure-bound
# local, a function-typed parameter, a copy of either, a function reference
# or a returned closure was never checked:
#     let g=fn(a:i64;b:i64):i64{<a+b};  g(1;2;3;4)   -- --check clean
#     g(1)  and  g("x";2)                            -- --check clean
# and each then built and ran (printing 3, 1 and an address).  TY_FUNC
# carried only a return type, so there was nothing to check against.
#
# NOW: TY_FUNC built from `(T1;T2):R`, a closure literal, `&name` or a bare
# function name records its parameter types.  A call through such a value
# draws E4026 (argument count) or E4031 (argument type) at the call, with NO
# `fix` field — whether an argument is missing, surplus or mistyped is not
# decidable from the call (AGENTS.md §3.1).  Passing a function value whose
# signature disagrees with a function-typed parameter is E4031.
#
# Negative control: remove the `124.0c item 2` block at the top of
# NODE_CALL_EXPR in infer_impl (types.c) — cases 1-8 go green-to-red (no
# diagnostic); case 11 (bare function name to a `(i64):i64` parameter) stays
# clean either way, and went red when the bare name was a signature-less
# TY_FUNC (the first draft of this change).
#
# Story: 124.0c (item 2)

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
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
# every diagnostic line that carries a "fix" key
fixes() { "${TKC}" --check "$1" 2>&1 | awk '/"error_code"/ && /"fix"/{n++} END{print n+0}'; }

prog() {   # NAME BODY-LINES... -> ${WORK}/NAME.tk
    local n="$1"; shift
    { echo 'm=t;i=io:std.io;'
      echo 'f=add(a:i64;b:i64):i64{<a+b};'
      echo 'f=inc(x:i64):i64{<x+1};'
      echo 'f=mk(n:i64):(i64):i64{<fn(x:i64):i64{<x+n}};'
      echo 'f=ap(g:(i64):i64;x:i64):i64{<g(x)};'
      printf '%s\n' "$@"; } > "${WORK}/$n.tk"
}

# 1-3. a closure-bound local: too many, too few, a mistyped argument
prog p1 'f=main():i64{let g=fn(a:i64;b:i64):i64{<a+b};<g(1;2;3;4)};'
expect "1-closure-local-too-many" "$(codes "${WORK}/p1.tk")" "E4026 "
prog p2 'f=main():i64{let g=fn(a:i64;b:i64):i64{<a+b};<g(1)};'
expect "2-closure-local-too-few" "$(codes "${WORK}/p2.tk")" "E4026 "
prog p3 'f=main():i64{let g=fn(a:i64;b:i64):i64{<a+b};<g("x";2)};'
expect "3-closure-local-arg-type" "$(codes "${WORK}/p3.tk")" "E4031 "

# 4. a function-typed parameter
prog p4 'f=ap2(g:(i64):i64;x:i64):i64{<g(x;1)};' 'f=main():i64{<ap2(&inc;1)};'
expect "4-fn-typed-param" "$(codes "${WORK}/p4.tk")" "E4026 "

# 5. a copy of a closure
prog p5 'f=main():i64{let g=fn(a:i64;b:i64):i64{<a+b};let h=g;<h(1)};'
expect "5-copied-closure" "$(codes "${WORK}/p5.tk")" "E4026 "

# 6. a function reference bound to a local
prog p6 'f=main():i64{let r=&add;<r(1;2;3)};'
expect "6-fn-reference-local" "$(codes "${WORK}/p6.tk")" "E4026 "

# 7. a closure returned by a call
prog p7 'f=main():i64{let m=mk(3);<m("s")};'
expect "7-returned-closure-arg-type" "$(codes "${WORK}/p7.tk")" "E4031 "

# 8. a closure's own function-typed parameter, inside an interpolation
prog p8 'f=main():i64{let k=fn(q:(str):i64):i64{io.println("\(q(1))");<0};<0};'
expect "8-closure-param-in-interp" "$(codes "${WORK}/p8.tk")" "E4031 "

# 9. none of these diagnostics carries a fix
expect "9-no-fix-field" "$(fixes "${WORK}/p1.tk") $(fixes "${WORK}/p3.tk") $(fixes "${WORK}/p5.tk")" "0 0 0"

# 10. a closure of the wrong signature passed to a function-typed parameter
prog p10 'f=main():i64{let s=fn(x:str):i64{<1};<ap(s;1)};'
expect "10-wrong-signature-argument" "$(codes "${WORK}/p10.tk")" "E4031 "

# 11-12. positive: every well-typed form is clean — closure, reference, bare
#    function name and returned closure through a (i64):i64 parameter
prog p11 'f=main():i64{let n=2;let g=fn(x:i64):i64{<x+n};let h=g;let m=mk(1);' \
         '  <ap(g;1)+ap(&inc;1)+ap(inc;1)+ap(m;1)+h(1)+m(1)+ap(fn(x:i64):i64{<x};0)};'
expect "11-well-typed-calls-clean" "$(codes "${WORK}/p11.tk")" ""
prog p12 'f=main():i64{let r=&add;let r2=r;let q=fn(a:i64;b:i64):i64{<a*b};<r2(1;2)+q(2;3)};'
expect "12-well-typed-arity-clean" "$(codes "${WORK}/p12.tk")" ""

echo "C190: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
