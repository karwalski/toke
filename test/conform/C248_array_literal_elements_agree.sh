#!/usr/bin/env bash
# C248_array_literal_elements_agree.sh — every element of an array literal is
# type-checked and must agree with the first (story 138.9, loke T-24).
#
# Only the FIRST element of `@(..)` was inferred: the rest were neither
# type-checked nor compared with it.  `@(st.items;e)` -- an @($entry) and an
# $entry, written where an append was meant -- built a two-element array of
# mixed kinds, length 2 whatever st.items held.  It is E4031 now; an integer
# literal still narrows (`@(x;1)` on a u8), and an unknown type is not judged.
#
# Negative control: TKC=<main's toke> -- cases 1-4 fail (rc=0).
#
# Story: 138.9 (T-24)

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

expect "1 array and element mixed (loke T-24)" \
    "$(diags 'm=q;t=$entry{scope:str};t=$state{items:@$entry};f=add(st:$state;e:$entry):$state{<$state{items:@(st.items;e)}};f=main():i64{<0};')" "E4031|entry|ABSENT"
expect "2 i64 then f64" \
    "$(diags 'm=q;f=main():i64{let a=@(1;2.5);<0};')" "E4031|f64|ABSENT"
expect "3 str then i64" \
    "$(diags 'm=q;f=main():i64{let a=@("a";1);<0};')" "E4031|i64|ABSENT"
expect "4 a later element is itself type-checked" \
    "$(diags 'm=q;f=main():i64{let a=@(1;2+true);<0};')" "E4031|bool|ABSENT"
expect "5 homogeneous literals stay clean" \
    "$(diags 'm=q;f=main():i64{let a=@(1;2;3);let s=@("a";"b");let n=@(@(1);@(2;3));<0};')" "rc=0"
expect "6 a literal narrows to a u8 element" \
    "$(diags 'm=q;f=g(x:u8):i64{let a=@(x;1);let b=@(1;x);<0};f=main():i64{<0};')" "rc=0"

echo "C248: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
