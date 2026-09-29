#!/usr/bin/env bash
# C249_no_equality_on_struct_or_sum.sh — `==` / `!=` on two struct or sum
# values is refused, not miscompiled (story 138.9, loke T-14).
#
# The emitter has no structural equality for composites: it compares the two
# handles (`x==$lvl{$restricted:0}` with x returned from a function is never
# true) or runs tk_str_cmp over the boxes (the tag word read as a C string,
# the payload ignored).  Both answers are wrong, so the checker says so.  An
# opaque .tki handle and `$none` keep their handle comparison.
#
# Negative control: TKC=<main's toke> -- cases 1-3 fail (rc=0).
#
# Story: 138.9 (T-14)

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

expect "1 a returned sum against a literal (loke T-14)" \
    "$(diags 'm=q;t=$lvl{$public:i64;$restricted:i64};f=mk():$lvl{<$lvl{$restricted:0}};f=main():i64{let x=mk();if(x==$lvl{$restricted:0}){<1};<0};')" "E4031|lvl|ABSENT"
expect "2 two sum locals" \
    "$(diags 'm=q;t=$lvl{$public:i64;$restricted:i64};f=main():i64{let a=$lvl{$restricted:0};let b=$lvl{$restricted:1};if(a!=b){<1};<0};')" "E4031|lvl|ABSENT"
expect "3 two struct locals" \
    "$(diags 'm=q;t=$pt{x:i64};f=main():i64{let a=$pt{x:1};let b=$pt{x:1};if(a==b){<1};<0};')" "E4031|pt|ABSENT"
expect "4 comparing fields stays clean" \
    "$(diags 'm=q;t=$pt{x:i64};f=main():i64{let a=$pt{x:1};let b=$pt{x:1};if(a.x==b.x){<1};<0};')" "rc=0"

echo "C249: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
