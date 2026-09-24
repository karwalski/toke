#!/usr/bin/env bash
# C044_rebind_in_force.sh — a use of a re-bound name is typed against the
# binding in force at that point, not the first binding in the function.
#
# THE DEFECT. The checker's NODE_IDENT lookup used find_binding_node, which
# returns the FIRST binding of a name anywhere in the function body. toke
# permits re-binding a name in the same scope with a different type
# (test/standalone/test_shadow_retype.tk), so after
#
#     let s=@(1;2); ...; let s="abc"; io.println(s);
#
# `io.println(s)` was checked against the array: a false E4031 ("expected
# 'str', got '@(i64)'") on a correct program, present at 444a55c. 127.157
# typed string-literal bindings, which created the mirror image
# (`let s="abc"; ...; let s=@(1;2); s.get(0)` → "cannot index into 'str'").
#
# WHAT IS ASSERTED:
#   1. Both orders of a same-scope retype --check clean and run correctly.
#   2. A self-referencing re-bind (`let x=x+1` style, here `let n=n.len`)
#      resolves its initialiser to the PREVIOUS binding, not to itself.
#   3. The binding in force is still enforced: using the later (array)
#      binding as a str is still E4031 — the fix picks a binding, it does
#      not stop checking.
#
# Negative control (measured): TKC built from 444a55c fails 1b and 3 (3
# because the first binding, a string literal, was TY_UNKNOWN there); TKC
# built from the 127.157 commit without this fix fails 1a, 1b and 3.
#
# Story: 127.157 (exposed by it; the array-then-str half predates it)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
run() { "${TKC}" "$1" -o "${WORK}/bin" >/dev/null 2>&1 && "${WORK}/bin" 2>/dev/null | tr '\n' '|'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

prog() { printf 'm=t;\ni=io:std.io;\nf=main():i64{\n%s\n  <0\n};\n' "$2" > "${WORK}/$1.tk"; }

prog strfirst '  let s="abc";
  io.println(s);
  let s=@(1;2);
  let c=s.get(1);
  io.println("\(c)");'
prog arrfirst '  let s=@(1;2);
  let c=s.get(0);
  io.println("\(c)");
  let s="abc";
  io.println(s);'
prog selfref  '  let n="abcd";
  let n=n.len;
  io.println("\(n)");'
prog stillchecked '  let s="abc";
  io.println(s);
  let s=@(1;2);
  io.println(s);'

expect "1a-str-then-array-checks" "$(codes "${WORK}/strfirst.tk")" ""
expect "1a-str-then-array-runs"   "$(run "${WORK}/strfirst.tk")" "abc|2|"
expect "1b-array-then-str-checks" "$(codes "${WORK}/arrfirst.tk")" ""
expect "1b-array-then-str-runs"   "$(run "${WORK}/arrfirst.tk")" "1|abc|"
expect "2-self-reference-checks"  "$(codes "${WORK}/selfref.tk")" ""
expect "2-self-reference-runs"    "$(run "${WORK}/selfref.tk")" "4|"
expect "3-binding-in-force-still-enforced" "$(codes "${WORK}/stillchecked.tk")" "E4031 "

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
