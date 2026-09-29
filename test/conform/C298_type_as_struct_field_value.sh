#!/usr/bin/env bash
# C298_type_as_struct_field_value.sh — a type standing as a struct field's
# VALUE is E4033 at --check (story 138.13 / GB's finding, loke T-13 family).
#
# `$w{l:$internal}` passed --check; codegen rejects it with E4033 at build
# (GB).  The checker says so where bind, assignment and argument already did
# (127.227).
#
# Negative control: TKC=<main's toke> -- case 1 fails (rc=0).
#
# Story: 138.13

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

expect "1 a type as a field value" \
    "$(diags 'm=q;t=$w{l:i64};t=$internal{a:i64};f=main():i64{let x=$w{l:$internal};<0};')" "E4033|\$internal|ABSENT"
expect "2 a value stays clean" \
    "$(diags 'm=q;t=$w{l:i64};f=main():i64{let x=$w{l:3};<x.l};')" "rc=0"
expect "3 a sum variant literal stays clean" \
    "$(diags 'm=q;t=$lvl{$a:i64;$b:i64};f=main():i64{let x=$lvl{$a:0};<0};')" "rc=0"

echo "C298: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
