#!/usr/bin/env bash
# C219_did_you_mean_skips_internal.sh — the E4027 "did you mean" never
# suggests an export the interface marks `"internal": true`
# (story 127.220 item 7).
#
# tki_closest_member scored every recorded export of the alias, so
# `arr.arrayappnd(...)` was told "did you mean 'arr.arrayappend'?" —
# array.arrayappend is a glue helper (`"internal": true` in array.tki), not a
# member to steer an author to.  names.c now records the flag and the
# suggestion skips it; with no other candidate the generic text stands.
#
# Negative control: TKC=<main's toke> -- case 1 fails.
#
# Story: 127.220 (item 7)

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
fixof() {  # $1 = call; prints code|fix of the first diagnostic
    printf 'm=q;\ni=arr:std.array;\nf=main():i64{\n  <%s\n};\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    print("%s|%s" % (d.get("error_code"), d.get("fix", "ABSENT"))); break'; }

expect "1 an internal export is not suggested" \
    "$(fixof 'arr.arrayappnd(0;0)')" "E4027|check the module's interface for the correct member name"
expect "2 a public export still is" \
    "$(fixof 'arr.indxof(0;0)')" "E4027|did you mean 'arr.indexof'?"

echo "C219: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
