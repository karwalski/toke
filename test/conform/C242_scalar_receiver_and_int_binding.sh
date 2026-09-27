#!/usr/bin/env bash
# C242_scalar_receiver_and_int_binding.sh — an integer-literal binding is an
# i64, and a scalar answers no receiver method or property (story 127.229
# item 1, 127.230 item 2).
#
# `let x=5` was TY_UNKNOWN to the checker, so nothing judged it:
#   * `x.len`, `x.slice(0;1)`, `x.charat(0)`, `x.get(0)` passed --check and
#     read the i64 as a pointer at run time (garbage or SIGBUS);
#   * `let b=3; g(b)` into a str parameter was clean while `g(3)` was E4031.
#
# The untyped handle is deliberately NOT judged: `let x=j.parse(..)` stays
# TY_UNKNOWN (at run time it may be an array or a str), so `x.len` and
# `x.get(0)` on it remain clean -- case 7 pins that decision.
#
# Negative control: TKC=<main's toke> -- cases 1-6 and 9-11 fail (rc=0).
#
# Story: 127.229 (item 1), 127.230 (items 2 and 7)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|got|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
H='m=q;i=j:std.json;i=s:std.str;'

expect "1 .len on an int-literal binding" \
    "$(diags "${H}"'f=main():i64{let x=5;j.print(x.len);<0};')" "E4036|i64.len|ABSENT"
expect "2 .slice on an int-literal binding" \
    "$(diags "${H}"'f=main():i64{let x=5;j.print(x.slice(0;1));<0};')" "E4036|i64.slice|ABSENT"
expect "3 .charat on a negated int-literal binding" \
    "$(diags "${H}"'f=main():i64{let x=-5;j.print(x.charat(0));<0};')" "E4036|i64.charat|ABSENT"
expect "4 .get(0) on an int-literal binding" \
    "$(diags "${H}"'f=main():i64{let x=5;j.print(x.get(0));<0};')" "E4031|i64|indexing with [] requires an array type"
expect "5 an int-literal binding into a str parameter" \
    "$(diags "${H}"'f=g(v:str):i64{<0};f=main():i64{let b=3;<g(b)};')" "E4031|i64|ABSENT"
expect "6 a method on an annotated f64" \
    "$(diags "${H}"'f=main():i64{let y:f64=1.5;j.print(y.contains(1));<0};')" "E4036|f64.contains|ABSENT"
expect "7 the untyped j.parse handle is not judged" \
    "$(diags "${H}"'f=main():i64{let x=j.parse(s.argv(1));j.print(x.len);j.print(x.get(0));<0};')" "rc=0"
expect "8 an int binding used as an int stays clean" \
    "$(diags "${H}"'f=main():i64{let n=3;let a=@(1;2;3);let m=mut.0;m=m+n;lp(let i=0;i<a.len;i=i+1){m=m+a.get(i)};<m};')" "rc=0"

# 127.230 item 7: the same hole for a float literal.
expect "9 a float-literal binding into an i64 parameter" \
    "$(diags "${H}"'f=g(x:i64):i64{<x};f=main():i64{let a=2.5;<g(a)};')" "E4031|f64|ABSENT"
expect "10 a float-literal binding in an i64 initialiser" \
    "$(diags "${H}"'f=main():i64{let a=2.5;let c:i64=a+1.0;<c};')" "E4031|f64|ABSENT"
expect "11 a negated float-literal binding into an i64 parameter" \
    "$(diags "${H}"'f=g(x:i64):i64{<x};f=main():i64{let d=-0.5;<g(d)};')" "E4031|f64|ABSENT"
expect "12 a float binding used as a float stays clean" \
    "$(diags "${H}"'f=h(x:f64):f64{<x};f=main():i64{let a=2.5;let b=h(a)*2.0;<0};')" "rc=0"

echo "C242: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
