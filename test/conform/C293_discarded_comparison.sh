#!/usr/bin/env bash
# C293_discarded_comparison.sh — a comparison standing as a statement is
# E4074 (story 137.14, 138.10 / loke T-26).
#
# `x.a==9;` -- `==` written for the assignment `=` -- compiled as a silent
# no-op.  A comparison statement computes a bool and drops it.  The last
# statement of a block is not judged: it may be the block's value (an
# expression-if), and a function body's tail is E4071's.
#
# Negative control: TKC=<main's toke> -- cases 1-3 fail (rc=0).
#
# Story: 137.14, 138.10 (T-26)

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

expect "1 == as a statement (loke T-26)" \
    "$(diags 'm=q;t=$r{a:i64};f=main():i64{let x=mut.$r{a:1};x.a==9;<x.a};')" "E4074|a discarded comparison|ABSENT"
expect "2 < as a statement" \
    "$(diags 'm=q;f=main():i64{let a=1;a<2;<a};')" "E4074|a discarded comparison|ABSENT"
expect "3 != inside a loop body" \
    "$(diags 'm=q;f=main():i64{lp(let i=0;i<3;i=i+1){i!=1;let z=i};<0};')" "E4074|a discarded comparison|ABSENT"
expect "4 a comparison as the value of an expression-if stays clean" \
    "$(diags 'm=q;f=main():i64{let a=1;let c=if(a>0){a==1}el{false};<0};')" "rc=0"
expect "5 an assignment stays clean" \
    "$(diags 'm=q;f=main():i64{let v=mut.1;v=9;<v};')" "rc=0"

echo "C293: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
