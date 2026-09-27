#!/usr/bin/env bash
# C151_type_errors_after_name_errors.sh — a file with name-resolution errors
# still reports type errors in functions the name errors did not touch, and
# never a cascade (story 123.12c, 84.1.8/9).
#
# THE GAP.  Within one phase the compiler already reported every error (up to
# 20), but any name-resolution error stopped compilation before the type
# checker, so a type error elsewhere surfaced only on the next compile.
#
# THE RULE (src/main.c, typecheck_after_name_errors).  Continue into the type
# checker only if every name error lies in the BODY of a top-level function
# (at or after its first statement).  That function is tainted: its
# diagnostics are dropped.  Only errors inside untainted functions are kept.
# A name error in a signature, a duplicate definition, or anything outside a
# function body means no continuation -- as before.
#
# Cases:
#   1. undeclared names in two bodies + a type error in a third function
#      -> E3011, E3011, E4031 (the E4031 is new)
#   2. the tainted function's own cascade is NOT reported: `let s:str=zzz`
#      with zzz undeclared gives exactly one E3011, no E4031
#   3. name error in a signature -> no continuation (no E4031)
#   4. duplicate definition (E3012 at the name) -> no continuation
#   5. JSON records keep the schema: the recovered E4031 carries stage
#      type_check and the D-id sequence is contiguous (D0001..D0003)
#   6. exit status is 1 in every case
#
# Negative control: make recover_keep ignore the taint array -> case 2 gains
# the tainted body's E4031 (and the 400-program corpus measurement in the
# 123.12 report goes from 0 to 4 unexpected results per variant).  Make
# fn_body_start return the STMT_LIST's own start -> cases 3 and 4 fail.
#
# Story: 123.12

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d /tmp/tkc_c151_XXXXXX)"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"

echo "C151: type errors after name errors, without cascades (123.12c)"

# run FILE -> writes out.json, sets RC and GOT (error codes in emission order)
run() {
    "${TKC}" --check --diag-json "$1" > out.json 2>&1; RC=$?
    GOT="$(sed -n 's/.*"error_code":"\([EW][0-9]*\)".*/\1/p' out.json | tr '\n' ' ' | sed 's/ $//')"
}
expect() {  # name file want-codes
    run "$2"
    if [ "${GOT}" = "$3" ] && [ "${RC}" -eq 1 ]; then
        echo "  PASS: $1"; PASS=$((PASS+1))
    else
        echo "  FAIL: $1: want '$3' rc 1, got '${GOT}' rc ${RC}"; sed 's/^/      /' out.json | cut -c1-160 | head -5; FAIL=$((FAIL+1))
    fi
}

cat > two_bodies.tk <<'X'
m=main;
f=a():i64{
  <zzz
};
f=b():i64{
  <qqq
};
f=main():i64{
  let s:str=5;
  <0
};
X
expect "name errors in two bodies, type error in a third" two_bodies.tk "E3011 E3011 E4031"

cat > cascade.tk <<'X'
m=main;
f=a():i64{
  let s:str=zzz;
  let t:bool=7;
  <0
};
f=main():i64{
  <0
};
X
expect "tainted body reports nothing beyond its name error" cascade.tk "E3011"

cat > sig.tk <<'X'
m=main;
f=a(x:$nosuch):i64{
  <0
};
f=main():i64{
  let s:str=5;
  <0
};
X
expect "name error in a signature: no continuation" sig.tk "E3011"

cat > dup.tk <<'X'
m=main;
f=a():i64{
  <0
};
f=a():i64{
  <1
};
f=main():i64{
  let s:str=5;
  <0
};
X
expect "duplicate definition: no continuation" dup.tk "E3012"

run two_bodies.tk
ids="$(sed -n 's/.*"diagnostic_id":"\(D[0-9]*\)".*/\1/p' out.json | tr '\n' ' ')"
st="$(grep '"error_code":"E4031"' out.json | sed -n 's/.*"stage":"\([a-z_]*\)".*/\1/p')"
if [ "${ids}" = "D0001 D0002 D0003 " ] && [ "${st}" = "type_check" ]; then
    echo "  PASS: recovered record keeps the schema (ids ${ids}stage ${st})"; PASS=$((PASS+1))
else
    echo "  FAIL: ids '${ids}' stage '${st}'"; FAIL=$((FAIL+1))
fi

echo "C151: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
