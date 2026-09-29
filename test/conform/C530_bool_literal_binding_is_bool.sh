#!/usr/bin/env bash
# C530_bool_literal_binding_is_bool.sh — `let b=true; b.len()` is a checker
# error, not a codegen crash (story 127.248 item 1).
#
# THE DEFECT.  bind_init_type() typed an un-annotated `let` from an int, a
# float and a string literal, but held the bool literal out, so `let b=true`
# was TY_UNKNOWN and 127.229 item 1's scalar-receiver rule (E4036) never saw
# a bool it could judge.  `b.len()` passed --check and died in codegen with
# E9003 (`'%t1' defined with type 'i1' but expected 'i64'`); `b.len` the same
# way.  A bool PARAMETER and a bound comparison (`let b=1<2`) were already
# judged; `mdns.browse(..)` (declared bool) is judged at HEAD (case 4).
#
# Negative control: TKC=<main d3a083b8's toke> -- cases 1 and 2 fail (rc=0 at
# --check, no E4036).  Cases 3-5 are green on both.
#
# Story: 127.248
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
# codes SRC: the E-codes --check emits, each followed by a space, then "rc=N".
codes() { printf '%s\n' "$1" > p.tk
    local out rc; out="$("${TKC}" --check p.tk 2>&1)"; rc=$?
    echo "${out}" | sed -n 's/.*"error_code":"\(E[0-9]*\)".*/\1/p' | tr '\n' ' '
    echo "rc=${rc}"; }

expect "1 let b=true; b.len()" "$(codes 'm=t;
i=io:std.io;
f=main():i64{
  let b=true;
  let n=b.len();
  io.println("\(n)");
  <0
};')" "E4036 rc=1"

expect "2 let b=false; b.len" "$(codes 'm=t;
f=main():i64{
  let b=false;
  <b.len as i64
};')" "E4036 rc=1"

expect "3 control: a bool parameter" "$(codes 'm=t;
f=h(b:bool):u64{ <b.len() };
f=main():i64{ <h(true) as i64 };')" "E4036 rc=1"

expect "4 control: mdns.browse is declared bool" "$(codes 'm=t;
i=mdns:std.mdns;
f=main():i64{
  let r=mdns.browse("_http._tcp");
  <r.len() as i64
};')" "E4036 rc=1"

# 5. A bool-literal binding still does everything a bool does: a condition,
#    interpolation, and the owner's bool -> i64 widening into a parameter.
printf '%s\n' 'm=t;
i=io:std.io;
f=g(x:i64):i64{<x};
f=main():i64{
  let b=true;
  let c=false;
  if(b&&!c){io.println("t")};
  io.println("\(b) \(c)");
  <g(b)+g(c)
};' > ok.tk
"${TKC}" --allow-all --out ok ok.tk >/dev/null 2>&1
expect "5 control: a bool-literal binding still works" "$(./ok 2>&1 | tr '\n' '|'; echo "rc=$?")" "t|true false|rc=1"

echo "C530: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
