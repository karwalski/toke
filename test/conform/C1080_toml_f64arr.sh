#!/usr/bin/env bash
# C1080_toml_f64arr.sh — std.toml gains toml.f64arr (story 113.B.4 remainder).
#
# THE GAP.  113.B.4 shipped strarr/i64arr/boolarr/sectionarr but withheld
# toml.f64arr: the checker left the $ok binding of a stdlib `@f64!E` call
# untyped, so `.get(i)` read the double's bit pattern as an integer (127.297,
# fixed by Batch Z83 7e0bd556).  With that fixed, the export is added.
#
# THE SURFACE (docs/spec/stdlib-signatures.md §std.toml).
#   toml.f64arr(v:$tomlval;key:$str):@f64!$tomlerr
# The glue stores each double's bit pattern in its i64 slot, as tk_array.h
# lays out @f64; an element is accepted exactly when toml.f64 would accept it
# (a float, or a decimal integer as the nearest double).
#
# Document: r=[1.5,-0.25,2] n=[1,2] m=[1.5,"x"] e=[] name="x"
# Cases (one output line each):
#   1 unannotated mt binding: r.get(0)*2.0             -> 3
#   2 len and the other elements (interpolated)        -> "3 -0.25 2"
#   3 the array passed to a user f(a:@f64) that sums it -> 3.25
#   4 annotated `let q:@f64 = mt ...`, integer elements -> 2.5
#   5 mixed array                                      -> array element 1 is not a float
#   6 empty array: $ok, len 0                          -> 0
#   7 absent key                                       -> key not found
#   8 a string value                                   -> key is not an array
#   9 the IR reads the element as a double: no sitofp in main's lowering
#  10 toml.f64arr(cfg; 1) -> E4031 at --check (the key parameter is $str)
#
# Negative control: C1080_STDLIB=<main 5eb15d9a's src/stdlib> (the interfaces
# still come from this tree) -- 1/10: the glue symbol tk_toml_f64arr_w is
# absent, so cases 1-8 and 10 fail on "'toml.f64arr' has no implementation";
# case 9 (IR only, no link) passes there, because the typing is 127.297's.
#
# Story: 113.B.4
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1080_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1080_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'EOF'
m=main;i=io:std.io;i=toml:std.toml;i=s:std.str;
f=sum(a:@f64):f64{ let t=mut.0.0; let i=mut.0; lp(i<a.len){ t=t+a.get(i); i=i+1 }; <t };
f=main():i64{
  let cfg = mt toml.load("r = [1.5, -0.25, 2]\nn = [1, 2]\nm = [1.5, \"x\"]\ne = []\nname = \"x\"\n") {$ok:v v;$err:e < 1};
  let r = mt toml.f64arr(cfg; "r") {$ok:v v;$err:e < 2};
  let x = r.get(0);
  io.println(s.fromfloat(x*2.0));
  io.println("\(r.len) \(r.get(1)) \(r.get(2))");
  io.println(s.fromfloat(sum(r)));
  let q:@f64 = mt toml.f64arr(cfg; "n") {$ok:v v;$err:e @()};
  io.println(s.fromfloat(q.get(1)+0.5));
  io.println(mt toml.f64arr(cfg; "m") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.f64arr(cfg; "e") {$ok:v s.fromint(v.len as i64);$err:e e.msg});
  io.println(mt toml.f64arr(cfg; "zz") {$ok:v "ok";$err:e e.msg});
  io.println(mt toml.f64arr(cfg; "name") {$ok:v "ok";$err:e e.msg});
  <0
};
EOF
if "${TKC}" --out "${WORK}/p" "${WORK}/p.tk" >"${WORK}/build.txt" 2>&1; then
    out="$("${WORK}/p" 2>&1)"
else
    out="BUILD FAILED: $(grep -o '"message":"[^"]*"' "${WORK}/build.txt" | grep -v lossy | head -1)"
fi
line() { printf '%s\n' "${out}" | sed -n "${1}p"; }
expect "1-unannotated-get-times-two"   "$(line 1)" "3"
expect "2-len-and-elements"            "$(line 2)" "3 -0.25 2"
expect "3-passed-to-f64-param"         "$(line 3)" "3.25"
expect "4-annotated-integer-elements"  "$(line 4)" "2.5"
expect "5-mixed-array"                 "$(line 5)" "array element 1 is not a float"
expect "6-empty-array-ok-len-0"        "$(line 6)" "0"
expect "7-absent-key"                  "$(line 7)" "key not found"
expect "8-not-an-array"                "$(line 8)" "key is not an array"

if "${TKC}" --emit-llvm --out "${WORK}/p.ll" "${WORK}/p.tk" >/dev/null 2>&1; then
    got="sitofp=$(awk '/^define .*@main\(/,/^}/' "${WORK}/p.ll" | grep -c 'sitofp')"
else
    got="emit-failed"
fi
expect "9-ir-element-is-a-double" "${got}" "sitofp=0"

cat > "${WORK}/k.tk" <<'EOF'
m=main;i=toml:std.toml;
f=main():i64{
  let cfg = mt toml.load("r = [1.5]\n") {$ok:v v;$err:e < 1};
  let r = mt toml.f64arr(cfg; 1) {$ok:v v;$err:e < 2};
  <r.len as i64
};
EOF
got="$("${TKC}" --check "${WORK}/k.tk" 2>&1 | grep -o '"error_code":"E[0-9]*"' | head -1)"
expect "10-key-is-str" "${got}" '"error_code":"E4031"'

echo "C1080: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
