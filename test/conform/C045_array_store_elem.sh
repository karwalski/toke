#!/usr/bin/env bash
# C045_array_store_elem.sh — storing into an array whose element type is known
# must store that type, and the diagnostic is AT THE STORE.
#
# THE DEFECT. semantics.md §1.2: an array is an "ordered, homogeneous sequence
# of element type T". `let box=mut.@(0;0); box=box.set(0;src)` with `src` an
# @i64 stores an array into an i64 slot. The checker accepted that store and,
# once 127.129 (886bdb0) typed `box`, rejected the READ two statements later
# (`box.get(0).get(0)` → "cannot index into 'i64'"): accept the defect, blame
# a correct line. test/standalone/test_array_inplace.tk's container-escape
# case was exactly this and went BROKEN in diff-codegen; it now uses @@i64.
#
# WHAT IS ASSERTED:
#   1. `.set` / `.append` / `.push` of a wrong element type is E4031, and the
#      diagnostic's line is the store's line.
#   2. The same stores with the right element type — including an @@i64
#      holding arrays, and a u64 into an @i64 (the 127.114 widening) — are
#      clean and run.
#   3. Untyped receivers are left alone (`@()` has element type unknown).
#   4. An empty `fix` field: the checker cannot know whether the value, the element
#      type or the data structure is wrong (AGENTS.md §6).
#
# Negative control: TKC built from the commit before this one gives no E4031
# at the store (1a-1c red).
#
# Story: fix to 886bdb0 (127.129) regression, Batch E2

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# "<code>@<line>" for every diagnostic
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){c=substr($0,RSTART+14,RLENGTH-15); if(match($0,/"line":[0-9]+/)) c=c "@" substr($0,RSTART+7,RLENGTH-7); printf "%s ",c}'; }
run() { "${TKC}" "$1" -o "${WORK}/bin" >/dev/null 2>&1 && "${WORK}/bin" 2>/dev/null | tr '\n' '|'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

prog() { printf 'm=t;\ni=io:std.io;\nf=main():i64{\n%s\n  <0\n};\n' "$2" > "${WORK}/$1.tk"; }

# line 4 is the first body line
prog badset    '  let src=@(5;6);
  let box=mut.@(0;0);
  box=box.set(0;src);'
prog badappend '  let xs=mut.@(1;2);
  xs=xs.append("three");'
prog badpush   '  let fs=mut.@(1.5;2.5);
  fs=fs.push(3);'
prog good      '  let src=mut.@(5;6;7);
  let box=mut.@(@(0);@(0));
  box=box.set(0;src);
  src=src.set(0;88);
  let inner=box.get(0);
  let xs=mut.@(1;2);
  xs=xs.append(xs.len);
  let fs=mut.@(1.5);
  fs=fs.push(2.5);
  io.println("\(inner.get(0)) \(xs.get(2)) \(fs.len)");'
prog untyped   '  let xs=mut.@();
  xs=xs.append("a");
  io.println(xs.get(0));'

expect "1a-set-wrong-elem-at-the-store"    "$(codes "${WORK}/badset.tk")"    "E4031@6 "
expect "1b-append-wrong-elem-at-the-store" "$(codes "${WORK}/badappend.tk")" "E4031@5 "
expect "1c-push-int-into-f64-array"        "$(codes "${WORK}/badpush.tk")"   "E4031@5 "
expect "2-right-elem-checks"               "$(codes "${WORK}/good.tk")"      ""
expect "2-right-elem-runs"                 "$(run "${WORK}/good.tk")"        "5 2 2|"
expect "3-untyped-receiver-left-alone"     "$(codes "${WORK}/untyped.tk")"   ""
# No fix means no "fix" key at all (127.183).
expect "4-fix-field-empty" \
    "$("${TKC}" --check "${WORK}/badset.tk" 2>&1 | grep '"error_code"' | grep -vc '"fix":' || true)" "1"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
