#!/usr/bin/env bash
# C481_str_as_str_identity.sh — `x as str` on a value the compiler knows is a
# string is the same string, not its address (story 127.229 item 5).
#
# The cast lowering chose between "string identity" and "integer to decimal"
# by the LLVM type of the operand alone.  A str that crosses the i64 ABI --
# `t.slice(a;b)`, a let bound to one -- is an i64 there, so
# `"\(t.slice(0;1) as str)"` checked clean and printed the pointer
# (eval 0237).  The cast now asks expr_is_str_value(), the predicate the
# interpolation and match lowering already use, before converting a number.
#
# Negative control: TKC=<main's toke> -- cases 1-4 fail, each printing an
# address (4, `s.upper(t) as str`, is the same defect through a std call).
#
# Story: 127.229 (5)

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
cat > "${WORK}/p.tk" <<'TK'
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let t:str="abc";
  io.println("1 \(t.slice(0;1) as str)");
  let u=t.slice(0;1);
  io.println("2 \(u as str)");
  let b:str=t.slice(1;3) as str;
  io.println("3 \(b)");
  io.println("4 \(s.upper(t) as str)");
  io.println("5 \(t as str)");
  let n=5;
  io.println("6 \(n as str)");
  io.println("7 \(42 as str)");
  let f=2.5;
  io.println("8 \(f as str)");
  <0
};
TK
if ! "${TKC}" "${WORK}/p.tk" --out "${WORK}/p" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -5 "${WORK}/build.log"; exit 1
fi
OUT="$("${WORK}/p" 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "/^$1 /{s/^$1 //;p;q;}"; }
expect "1 an interpolated t.slice(..) as str is the slice" "$(line 1)" "a"
expect "2 a let bound to t.slice(..), as str" "$(line 2)" "a"
expect "3 let b:str=t.slice(..) as str" "$(line 3)" "bc"
expect "4 a str-returning std call as str" "$(line 4)" "ABC"
expect "5 a str local as str" "$(line 5)" "abc"
expect "6 an integer local as str is still its decimal" "$(line 6)" "5"
expect "7 an integer literal as str" "$(line 7)" "42"
expect "8 an f64 as str" "$(line 8)" "2.5"

echo "C481: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
