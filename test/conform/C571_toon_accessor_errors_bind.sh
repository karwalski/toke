#!/usr/bin/env bash
# C571_toon_accessor_errors_bind.sh — toon.str/i64/f64/bool/arr raise a real
# ToonErr box, so `$err:e` binds the variant and its reason (127.246 (2)).
#
# THE DEFECT.  toon_glue.c's five typed accessors signalled failure by storing
# the payload-less flag 1 in @tk_current_error.  The compiler took the error
# arm correctly, but the arm's binding was nil: `mt e {$missing:m …}` trapped
# RT005 and `e` could not be inspected.  toon.dec already raised a box
# (127.241); the accessors now do the same, with a message naming the call and
# the key (copied through tk_err_msg, since the key is the caller's string).
#
# Cases, on `user{name,age,ok,score}: Ann|notnum|maybe|x`:
#   1 toon.str  missing key       -> missing|toon.str: missing key: nokey
#   2 toon.i64  not a number      -> type|toon.i64: not an i64: age
#   3 toon.f64  not a number      -> type|toon.f64: not an f64: score
#   4 toon.bool not a bool        -> type|toon.bool: not a bool: ok
#   5 toon.arr  missing key       -> missing|toon.arr: missing key: zz
#   6 a success after the failures takes $ok (the slot is cleared): Ann
#   7 C375 check 4 (payload-less raises) lists no toon wrapper
#
# Negative control: C571_STDLIB=<main's src/stdlib at cde89021, stdlib/
# beside it> — the program dies at case 1 with "RT005: nil dereference"
# (all of 1–6 red); C375 with C375_ROOT on the same tree names the five
# toon wrappers under check 4.
#
# Story: 127.246 (2)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C571_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
TE='mt e {$parse:m s.concat("parse|";m);$type:m s.concat("type|";m);$missing:m s.concat("missing|";m)}'
cat > "${WORK}/p.tk" <<EOF
m=main;i=io:std.io;i=toon:std.toon;i=s:std.str;
f=main():i64{
  let t=mt toon.dec("user{name,age,ok,score}:
Ann|notnum|maybe|x") {\$ok:v v;\$err:e toon.empty()};
  io.println(mt toon.str(t;"nokey") {\$ok:v v;\$err:e ${TE}});
  io.println(mt toon.i64(t;"age") {\$ok:v s.fromint(v);\$err:e ${TE}});
  io.println(mt toon.f64(t;"score") {\$ok:v "okf";\$err:e ${TE}});
  io.println(mt toon.bool(t;"ok") {\$ok:v "okb";\$err:e ${TE}});
  io.println(mt toon.arr(t;"zz") {\$ok:v "oka";\$err:e ${TE}});
  io.println(mt toon.str(t;"name") {\$ok:v v;\$err:e ${TE}});
  <0
};
EOF
if ! "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; echo "C571: 0 passed, 1 failed"; exit 1
fi
OUT="$( (cd "${WORK}" && ./b) 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-str-missing"  "$(line 1)" "missing|toon.str: missing key: nokey"
expect "2-i64-type"     "$(line 2)" "type|toon.i64: not an i64: age"
expect "3-f64-type"     "$(line 3)" "type|toon.f64: not an f64: score"
expect "4-bool-type"    "$(line 4)" "type|toon.bool: not a bool: ok"
expect "5-arr-missing"  "$(line 5)" "missing|toon.arr: missing key: zz"
expect "6-ok-after-err" "$(line 6)" "Ann"
C375="$(C375_ROOT="$(cd "${TKC_STDLIB_DIR}/../.." && pwd)" bash "${SCRIPT_DIR}/C375_err_slot_glues.sh" 2>&1 | grep -c 'FAIL 4: tk_toon_')"
expect "7-c375-no-toon-payloadless" "${C375}" "0"
echo "C571: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
