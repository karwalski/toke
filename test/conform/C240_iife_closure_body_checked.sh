#!/usr/bin/env bash
# C240_iife_closure_body_checked.sh — an immediately-invoked closure's body is
# type-checked (story 127.230 item 3).
#
# `(fn():i64{<1+true})()` passed --check: every NODE_CALL_EXPR path inferred
# the arguments and never the callee, so a closure in callee position was not
# walked.  The row found it inside an interpolation; the interpolation was
# incidental -- the same IIFE bound with `let` was clean too.  Now the closure
# is walked (E4031 inside its body, located there) and the call is judged
# against its signature (arity E4026, argument E4031).
#
# Negative control: TKC=<main's toke> -- cases 1, 2, 3 and 4 fail.
#
# Story: 127.230 (item 3)

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
# code@col of each diagnostic, joined by ' / '; "rc=N" when there are none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s@%s" % (d.get("error_code"), d.get("pos", {}).get("col")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

expect "1 IIFE in an interpolation: body checked, located in the literal" \
    "$(diags 'm=q;i=io:std.io;f=main():i64{io.println("\((fn():i64{<1+true})())");<0};')" "E4031@56"
expect "2 IIFE bound by let: body checked" \
    "$(diags 'm=q;f=main():i64{let v=(fn():i64{<1+true})();<v};')" "E4031@36"
expect "3 IIFE arity against its signature" \
    "$(diags 'm=q;f=main():i64{<(fn(a:i64):i64{<a})()};')" "E4026@38"
expect "4 IIFE argument type against its signature" \
    "$(diags 'm=q;f=main():i64{<(fn(a:i64):i64{<a})("x")};')" "E4031@39"
expect "5 a well-typed IIFE stays clean" \
    "$(diags 'm=q;i=io:std.io;f=main():i64{io.println("\((fn(a:i64):i64{<a+1})(4))");<(fn():i64{<0})()};')" "rc=0"

echo "C240: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
