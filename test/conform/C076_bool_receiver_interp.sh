#!/usr/bin/env bash
# C076_bool_receiver_interp.sh — an element of a bool array and a predicate
# receiver method interpolate as `true`/`false`, like every other bool
# (story 127.194; owner decision 127.15; 127.94).
#
# THE DEFECT. Measured at main 2e932d6:
#
#     let a=@(true;false);  "\(a.get(0)) \(a.get(1))"   -> "1 0"
#     "\(s.contains("ell")) \(s.starts("x"))"           -> "1 0"
#     "\(m.contains("k"))"  "\(a.contains(9))"          -> "1"
#     "\(xs.contains(true))"                             -> E9003 from clang
#
# The first two shapes reach the backend as i64 0/1 (an array slot; the glue
# ABI), which 127.94's declared-bool tests (a .tki `bool` return, a struct
# field declared bool) could not see. The last is a separate defect in the
# same place: expr_llvm_type answered a one-argument receiver call with its
# ARGUMENT's type ("i1"), so the interpolation emitted `select i1` on an i64.
#
# THE RULE. contains / starts / ends / eq on a value receiver, and `.get(i)`
# on an array whose elements are bool, render as true/false. Integers are the
# control: `.get(i)` on an @i64, `.indexof`, `.len` and `.find` still print
# digits.
#
# 1c/1d: a function-returned @(bool) / @f64 is tagged like a parameter of that
# type (found by the corpus sweep: 1d printed 4609434218613702656 for 1.5).
#
# Negative control: TKC=<main 2e932d6 build> — cases 1-3 print 1/0 (1d prints
# bit patterns), case 4 fails to compile.
#
# Story: 127.194

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC}"; exit 1; fi
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

echo "C076: bool array elements and predicate receiver methods print true/false"
echo "--------------------------------------------------------------------------"

runs() {
    local name="$1" expected="$2" src="$3" out rc=0
    printf '%s\n' "${src}" > "${WORK}/p.tk"
    rm -f "${WORK}/p_bin"
    if ! "${TKC}" --allow-all --out "${WORK}/p_bin" "${WORK}/p.tk" > "${WORK}/c.log" 2>&1; then
        echo "FAIL ${name}: compile failed"
        awk 'match($0,/"error_code":"[A-Z][0-9]+"/){print "        " substr($0,RSTART,RLENGTH)}' "${WORK}/c.log" | head -1
        FAIL=$((FAIL + 1)); return
    fi
    out="$("${WORK}/p_bin" 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then echo "PASS ${name}"; PASS=$((PASS + 1))
    else echo "FAIL ${name}"; echo "        expected: ${expected}"; echo "        got:      ${out} (rc ${rc})"; FAIL=$((FAIL + 1)); fi
}

runs "1-bool-array-element" "true false" 'm=t;
i=io:std.io;
f=main():i64{
  let a=@(true;false);
  io.println("\(a.get(0)) \(a.get(1))");
  <0
};'

runs "1b-bool-array-parameter-element" "false true" 'm=t;
i=io:std.io;
f=show(b:@(bool)):str{
  <"\(b.get(0)) \(b.get(1))"
};
f=main():i64{
  io.println(show(@(false;true)));
  <0
};'

runs "1c-bool-array-returned-by-a-function" "true false" 'm=t;
i=io:std.io;
f=bs():@(bool){
  <@(true;false)
};
f=main():i64{
  let d=bs();
  io.println("\(d.get(0)) \(d.get(1))");
  <0
};'

runs "1d-f64-array-returned-by-a-function" "1.5 2" 'm=t;
i=io:std.io;
f=id(v:@f64):@f64{
  <v
};
f=main():i64{
  let a=id(@(1.5;2.0));
  io.println("\(a.get(0)) \(a.get(1))");
  <0
};'

runs "2-str-predicates" "true false true false true false" 'm=t;
i=io:std.io;
f=main():i64{
  let s="hello";
  io.println("\(s.contains("ell")) \(s.contains("z")) \(s.starts("he")) \(s.starts("x")) \(s.ends("lo")) \(s.eq("x"))");
  <0
};'

runs "3-map-and-array-contains" "true false true false" 'm=t;
i=io:std.io;
f=main():i64{
  let m=@("k":1);
  let a=@(1;2;9);
  io.println("\(m.contains("k")) \(m.contains("q")) \(a.contains(9)) \(a.contains(4))");
  <0
};'

runs "4-contains-with-a-bool-argument" "true" 'm=t;
i=io:std.io;
f=main():i64{
  let xs=@(true;false);
  io.println("\(xs.contains(true))");
  <0
};'

runs "5-control-integers-still-print-digits" "7 1 3 2" 'm=t;
i=io:std.io;
f=main():i64{
  let a=@(7;8;9);
  let s="hello";
  io.println("\(a.get(0)) \(a.indexof(8)) \(a.len) \(s.find("l"))");
  <0
};'

echo "--------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
