#!/usr/bin/env bash
# C305_match_arm_binding_scope.sh — a match arm's binding is scoped to its
# arm: it does not replace an outer binding of the same name for the rest
# of the function (reported by Batch LS; every match lowering).
#
# THE DEFECT. The arm binding is uniquified (`n.2`) and an alias n -> n.2
# pushed, and nothing popped it: after
#   let n=mt str.toint("150"){$ok:n n;$err:e 0-1};
#   let id=mt str.toint("7"){$ok:n n;$err:e 0-1};
# every later `n` read the second arm's slot and printed n=7 — a silent
# wrong value.  Blocks already pop their aliases (NODE_BLOCK); the three
# match lowerings (result, sum-tag, string chain) now do the same per arm.
#
# Negative control: TKC built from main at 9dad28f — cases 1-3 red
# ("n=7 id=7", "k=5105", "w=x").
#
# Story: 138 (LS report; filed by the main thread)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
run() { printf '%s\n' "$1" > "${WORK}/p.tk"; rm -f "${WORK}/b"
        "${TKC}" "${WORK}/p.tk" -o "${WORK}/b" >/dev/null 2>&1 || { echo build-failed; return; }
        "${WORK}/b" 2>/dev/null | tr '\n' ' '; }

expect "1-result-match" "$(run 'm=main;i=io:std.io;i=str:std.str;
f=main():i64{
  let n=mt str.toint("150"){$ok:n n;$err:e 0-1};
  let id=mt str.toint("7"){$ok:n n;$err:e 0-1};
  io.println("n=\(n) id=\(id)");
  <0
};')" "n=150 id=7 "

expect "2-sum-match" "$(run 'm=main;i=io:std.io;
t=$s{$a:i64;$b:i64};
f=pick(v:$s):i64{
  let k=mt v {$a:k k*1;$b:k k*2};
  let j=mt $s{$b:5} {$a:k k;$b:k k+100};
  <k*1000+j
};
f=main():i64{ io.println("k=\(pick($s{$a:2}))"); <0 };')" "k=2105 "

expect "3-string-match" "$(run 'm=main;i=io:std.io;
f=main():i64{
  let w="a";
  let s="x";
  let r=mt s {$y:w 1;$z:w 2;$x:w 3};
  io.println("w=\(w)");
  <0
};')" "w=a "

echo "C305: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
