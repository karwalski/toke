#!/usr/bin/env bash
# C880_xmod_f32_array_subscript.sh — an `@f32` array returned from another
# user module reads back its f32 values through `.get(i)` (story 127.313).
#
# THE DEFECT. lc.tk `f=f3():@f32{<@(1.5 as f32;2.5 as f32)}`, mc.tk
# `let a=lc.f3(); io.println("\(a.get(1))")` built and printed
# 4612811918334230528 (the double bit pattern of 2.5) at exit 0; arithmetic
# on the elements printed 9222246136947933184.  Elements are stored as
# double bits (fpext + bitcast).  A same-module call read them through the
# legacy @f64 local marker; across a module boundary the binding stayed
# untyped (repr_payload_type left f32 out, 127.308) and the emitter's
# subscript (llvm.c expr_llvm_type / emit_expr NODE_INDEX_EXPR) converted
# only an f64 or str element rtype.
#
# THE FIX. repr_payload_type adopts an array of f32 elements, and the
# subscript reads an f32 element rtype as a double, exactly as the marker
# path did.  A scalar f32 return is unchanged.
#
# Negative control: TKC built from main 7758dcf3 -- cases 1, 2 and 4 red
# (bit patterns); 3, 5, 6 and 7 green on both (an ordered compare of two
# positive bit patterns happens to agree).
#
# Story: 127.313
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c880_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# run <main source> [<library source>] -- prints the program's output on one line
run() {
    rm -f "${WORK}"/*.tk "${WORK}/b"
    printf '%s\n' "$1" > "${WORK}/mc.tk"
    local files=("${WORK}/mc.tk")
    if [ $# -ge 2 ]; then
        printf '%s\n' "$2" > "${WORK}/lc.tk"
        files=("${WORK}/lc.tk" "${WORK}/mc.tk")
    fi
    "${TKC}" -o "${WORK}/b" "${files[@]}" >/dev/null 2>&1 || { echo "build-failed"; return; }
    "${WORK}/b" 2>/dev/null | tr '\n' ' '
}
echo "C880: an @f32 array from another module reads its f32 elements (127.313)"
echo "---------------------------------------------------------------------------"
LC='m=lc;
f=f3():@f32{<@(1.5 as f32;2.5 as f32;4.0 as f32)};
f=half(x:f32):f32{<x/2.0 as f32};
f=s3():f32{<2.5 as f32};'
H='m=mc;i=io:std.io;i=lc:lc;'

expect "1 cross-module .get(i) interpolated" \
  "$(run "${H}f=main():i64{let a=lc.f3();io.println(\"\\(a.get(1))\");<0};" "$LC")" "2.5 "
expect "2 arithmetic on two elements" \
  "$(run "${H}f=main():i64{let a=lc.f3();let s=a.get(0)+a.get(1);io.println(\"\\(s)\");<0};" "$LC")" "4 "
expect "3 element into an f32 annotation, and as an f32 argument" \
  "$(run "${H}f=main():i64{let a=lc.f3();let x:f32=a.get(2);io.println(\"\\(x) \\(lc.half(a.get(2)))\");<0};" "$LC")" "4 2 "
expect "4 accumulated in a loop, and cast to f64" \
  "$(run "${H}f=main():i64{let a=lc.f3();let acc=mut.(0.0 as f32);lp(let i=0;i<a.len;i=i+1){acc=acc+a.get(i)};let d=a.get(1) as f64;io.println(\"\\(acc) \\(d)\");<0};" "$LC")" "8 2.5 "
expect "5 compared" \
  "$(run "${H}f=main():i64{let a=lc.f3();if(a.get(1)>a.get(0)){io.println(\"gt\")}el{io.println(\"le\")};<0};" "$LC")" "gt "
# controls: the same-module path, a local literal, and a scalar f32 return
expect "6 same-module @f32 return and a local @f32 literal" \
  "$(run 'm=mc;i=io:std.io;f=f3():@f32{<@(1.5 as f32;2.5 as f32)};f=main():i64{let a=f3();let l=@(0.5 as f32;0.25 as f32);io.println("\(a.get(1)) \(l.get(0)+l.get(1))");<0};')" "2.5 0.75 "
expect "7 a scalar f32 from another module" \
  "$(run "${H}f=main():i64{let s=lc.s3();io.println(\"\\(s)\");<0};" "$LC")" "2.5 "

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
