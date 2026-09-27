#!/usr/bin/env bash
# C070_test_eq_by_value.sh — `test.eq` is an assertion that can fail, and a
# str pair compares by value.
#
# Story: 127.68.
#
# THE DEFECT.  `tk_test_eq_w` was `return (a == b) ? 1 : 0` on the raw i64
# ABI value.  For two strings that is a comparison of their ADDRESSES, so
#
#     test.eq(str.upper("hello"); "HELLO")   -> 0
#
# for two strings that both print HELLO.  It printed nothing either way and
# marked nothing, and every call site discarded the result — so every
# assertion built on it was inert.  ooke had 157 (134.17) and wrote its own
# helpers rather than wait.
#
# THE RULE.  The integer meaning does not move: the same two values are
# compared and the same 1/0 is returned (section B pins that).  What is new
# is the assertion contract the rest of std.test keeps — a failure prints a
# `DIAGNOSTIC: kind=assertion_failed` line on stderr — and a str pair is
# compared by value.  The i64 ABI cannot tell a str handle from an integer,
# so the backend chooses from the static types; a pair it cannot type either
# way keeps the integer compare, which is the old behaviour rather than a new
# guess.  `test.eqstr` says "string" explicitly.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_testeq_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C070: test.eq is a real assertion, and compares strings by value"
echo "----------------------------------------------------------------"

# case NAME WANT_STDOUT WANT_DIAG SOURCE — build, run, and compare stdout
# exactly.  WANT_DIAG is the number of `kind=assertion_failed` lines stderr
# must carry: a passing assertion must be silent and a failing one must not.
case_() {
    local name="$1" want="$2" want_diag="$3" src="$4"
    printf '%s\n' "${src}" > "t.tk"
    rm -f t_bin
    if ! "${TKC}" --allow-all --out t_bin "t.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | tail -3
        FAIL=$((FAIL + 1))
        return
    fi
    local out rc=0 ndiag
    out="$(./t_bin 2>stderr.log)" || rc=$?
    ndiag="$(grep -c 'kind=assertion_failed' stderr.log || true)"
    if [ "${out}" = "${want}" ] && [ "${ndiag}" = "${want_diag}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        echo "        stdout expected: $(printf '%s' "${want}" | tr '\n' '|')"
        echo "        stdout got:      $(printf '%s' "${out}" | tr '\n' '|')  (rc ${rc})"
        echo "        assertion_failed lines: expected ${want_diag}, got ${ndiag}"
        sed 's/^/        stderr: /' stderr.log | head -3
        FAIL=$((FAIL + 1))
    fi
}

echo ""
echo "A. a str pair compares by value"
echo ""

# A1 — the row's own line.  Returned 0 before 127.68.
case_ "test.eq(str.upper(\"hello\");\"HELLO\") passes" "1" "0" \
'm=t;
i=test:std.test;
i=str:std.str;
i=io:std.io;
f=main():i64{
  if(test.eq(str.upper("hello");"HELLO")){io.println("1")}el{io.println("0")};
  <0
};'

# A2 — a let-bound str, not a call in argument position.
case_ "a let-bound str against a literal passes" "1" "0" \
'm=t;
i=test:std.test;
i=str:std.str;
i=io:std.io;
f=main():i64{
  let s=str.concat("HEL";"LO");
  if(test.eq(s;"HELLO")){io.println("1")}el{io.println("0")};
  <0
};'

# A3 — unequal strings must FAIL, and say so on stderr with both values.
case_ "unequal strings fail, with one assertion_failed line" "0" "1" \
'm=t;
i=test:std.test;
i=str:std.str;
i=io:std.io;
f=main():i64{
  if(test.eq(str.upper("hello");"WORLD")){io.println("1")}el{io.println("0")};
  <0
};'
if grep -q 'HELLO' stderr.log && grep -q 'WORLD' stderr.log; then
    echo "  PASS: the failure names both strings"
    PASS=$((PASS + 1))
else
    echo "  FAIL: the failure does not name both strings"
    sed 's/^/        stderr: /' stderr.log | head -2
    FAIL=$((FAIL + 1))
fi

# A4 — test.eqstr, the explicit spelling.
case_ "test.eqstr compares by value, both ways" "1
0" "1" \
'm=t;
i=test:std.test;
i=str:std.str;
i=io:std.io;
f=main():i64{
  if(test.eqstr(str.lower("ABC");"abc")){io.println("1")}el{io.println("0")};
  if(test.eqstr(str.lower("ABC");"abd")){io.println("1")}el{io.println("0")};
  <0
};'

echo ""
echo "B. integer uses keep their meaning"
echo ""

# B1 — equal and unequal integers return what they always returned; the only
# addition is the stderr line on the unequal one.
case_ "integers: 3==3 passes, 3==4 fails, computed 2+2==4 passes" "1
0
1" "1" \
'm=t;
i=test:std.test;
i=io:std.io;
f=main():i64{
  if(test.eq(3;3)){io.println("1")}el{io.println("0")};
  if(test.eq(3;4)){io.println("1")}el{io.println("0")};
  let n=2+2;
  if(test.eq(n;4)){io.println("1")}el{io.println("0")};
  <0
};'

# B2 — a .len result (an integer that comes from a str) is compared as an
# integer, not dispatched to the string compare.
case_ "str.len(s) against an integer is an integer compare" "1" "0" \
'm=t;
i=test:std.test;
i=str:std.str;
i=io:std.io;
f=main():i64{
  let s="hello";
  if(test.eq(str.len(s) as i64;5)){io.println("1")}el{io.println("0")};
  <0
};'

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
