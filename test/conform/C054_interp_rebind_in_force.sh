#!/usr/bin/env bash
# C054_interp_rebind_in_force.sh — the interpolation check judges a re-bound
# name by the binding in force at the literal, not the first one in the
# function (story 127.175; follows cc2ac3e / C044).
#
# THE DEFECT. check_interp_composites resolved a bare `\(x)` through
# find_binding_node, the FIRST binding of x in the function:
#
#     let s=@(1;2); io.println("\(s.len)"); let s="abc"; io.println("\(s)")
#
# drew E4032 "cannot interpolate a composite value" on line 7, where s is a
# str. It now uses find_binding_before with the literal as the use.
#
# NOT ASSERTED: that shape's BUILD. llvm.c's own interpolation lowering also
# types the re-bound s by its first binding and fails the build with a
# backstop E4032 reported at a wrapper-buffer offset (line 1). That is a
# codegen defect outside this story, reported separately; this suite pins
# the checker only.
#
# The other half of 127.175, recv_ident_is_bound, answers only "is this name
# bound at all" — a boolean that find_binding_node and find_binding_before
# (which falls back to it) give identically — so there is nothing to pin.
#
# Negative control: TKC built from 39b6cfd — case 1 is E4032 (red).
#
# Story: 127.175

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# code@line for each error
codes() { "${TKC}" --check "$1" 2>&1 | grep '"severity":"error"' | sed -E 's/.*"error_code":"([A-Z][0-9]+)".*"line":([0-9]+).*/\1@\2/' | tr '\n' ' '; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
prog() { printf 'm=t;\ni=io:std.io;\nf=main():i64{\n%s\n  <0\n};\n' "$2" > "${WORK}/$1.tk"; }

# 1. array first, str re-bind second: the str interpolation is clean
prog a '  let s=@(1;2);
  io.println("\(s.len)");
  let s="abc";
  io.println("\(s)");'
expect "1-str-in-force-no-E4032" "$(codes "${WORK}/a.tk")" ""
# 2. str first, array re-bind second: the array interpolation is still E4032
prog b '  let s="abc";
  io.println("\(s)");
  let s=@(1;2);
  io.println("\(s)");'
expect "2-array-in-force-is-E4032-on-line-7" "$(codes "${WORK}/b.tk")" "E4032@7 "
# 3. self-reference resolves to the PREVIOUS binding
prog c '  let s=@(1;2);
  let s="x\(s)";
  io.println(s);'
expect "3-self-reference-is-E4032" "$(codes "${WORK}/c.tk")" "E4032@5 "

echo "C054: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
