#!/usr/bin/env bash
# C217_array_contains_is_bool.sh — module-form `arr.contains(a;x)` is a bool
# (story 127.220 item 2).
#
# stdlib/array.tki declared `array.contains` as returning i64 (a gen_tki.py
# ABI placeholder, "gen":"abi"), so the backend read the interface and printed
# the module form as `1`/`0`, while the receiver form `a.contains(3)` printed
# `true`/`false`.  The glue (tk_arr_contains_w) returns 0/1: the return is a
# bool.  gen_tki.py keeps an existing record's hand-written types verbatim, so
# the interface is the source of this type; `gen_tki.py --check` stays clean.
#
# Negative control: TKC=<main's toke> TKC_STDLIB_DIR=<main's src/stdlib> (the
# interface is read beside the stdlib dir) -- case 1 prints "1 0|".
#
# Not covered here (a separate defect, reported): an UNANNOTATED `let b=` of
# any bool-returning call (`a.contains(3)`, `s.contains(x;"b")`) interpolates
# as 1 on main and on this branch alike.
#
# Story: 127.220 (item 2)

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
run() { rm -f "${WORK}/b"; "${TKC}" --out "${WORK}/b" "$1" > "${WORK}/build.log" 2>&1
        if [ -x "${WORK}/b" ]; then "${WORK}/b" 2>&1 | tr '\n' '|'; else echo "no binary"; fi; }

cat > "${WORK}/mf.tk" <<'EOF'
m=q;
i=io:std.io;
i=arr:std.array;
f=main():i64{
  let a=@(1;2;3);
  io.println("\(arr.contains(a;3)) \(arr.contains(a;9))");
  <0
};
EOF
expect "1 module form interpolates as true/false" "$(run "${WORK}/mf.tk")" "true false|"

cat > "${WORK}/rf.tk" <<'EOF'
m=q;
i=io:std.io;
i=arr:std.array;
f=main():i64{
  let a=@(1;2;3);
  io.println("\(a.contains(3)) \(a.contains(9))");
  if(arr.contains(a;2)){io.println("in")}el{io.println("out")};
  if(arr.contains(a;7)){io.println("in")}el{io.println("out")};
  <0
};
EOF
expect "2 receiver form agrees; the module form still tests as a condition" \
    "$(run "${WORK}/rf.tk")" "true false|in|out|"

"${TKC}" --check "${WORK}/mf.tk" >/dev/null 2>&1
expect "3 --check accepts the module form" "$?" "0"

echo "C217: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
