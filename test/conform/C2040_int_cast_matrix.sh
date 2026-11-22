#!/usr/bin/env bash
# C2040_int_cast_matrix.sh — every integer-to-integer `as` cast, at every
# width and signedness, builds and gives the value C gives (story 127.506,
# corpus-side pointer 131.131).
#
# HISTORY.  expr_llvm_type answers i64 for every sub-64-bit integer cast
# target (80.2.1: the narrowing path truncates, then re-extends for storage),
# but the identity path (`a as i32` on a u32, `a as u32` on an i32: both i32
# in LLVM) and the widening path (`a as i32` on an i16) left the value at the
# target's width.  The consumer -- a return, a let, an argument -- then
# emitted `trunc i64` on an i32 and clang refused the module (E9003).  Found
# by batch Z232; reproduced on 3.2.0 (a26e1e7b) and the wave integration
# branch.  Fixing it exposed a second difference from C: a signed source
# widened into an UNSIGNED target was zero-extended (`b as u16` on an i8 of
# -1 gave 255, while `b as i64 as u16` gave 65535); it now sign-extends, by
# the source's signedness, as C, Rust and Go do.
#
# WHAT IS ASSERTED
#   For each of the 64 (source, target) pairs over i8 i16 i32 i64 u8 u16 u32
#   u64, and 20 source values (each first cast from an i64 literal to the
#   source type), three consumers of `x as T`:
#     - returned from a function declared `:T`,
#     - interpolated in place,
#     - bound by an un-annotated let and interpolated;
#   1. the program builds (E9003 before)
#   2. its stdout is byte-for-byte the stdout of the same casts in C
#      ((T)(S)v, printed in decimal by the target's signedness)
#
# Negative control: TKC=<base tkc> TKC_STDLIB_DIR=<base>/src/stdlib fails 1
# (E9003), so 2 cannot run.
#
# Story: 127.506 (131.131)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2040_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT

python3 - "${WORK}/m.tk" "${WORK}/m.c" <<'PY'
import sys
T=[("i8","a","int8_t",1),("i16","b","int16_t",1),("i32","c","int32_t",1),("i64","d","int64_t",1),
   ("u8","e","uint8_t",0),("u16","f","uint16_t",0),("u32","g","uint32_t",0),("u64","h","uint64_t",0)]
V=["0","1","-1","127","128","200","255","256","-129","32767","32768","65535","65536","-32769",
   "2147483647","2147483648","4294967295","4294967296","-2147483649","9223372036854775807"]
tk=["m=main;","i=io:std.io;"]
tk+=[f"f=c{sl}{dl}(a:{s}):{d}{{<a as {d}}};" for s,sl,_,_ in T for d,dl,_,_ in T]
for s,sl,_,_ in T:
  body=[]
  for d,dl,_,_ in T:
    body.append(f'  let y{dl}=(x as {d});')
    body.append(f'  io.println("{s}>{d} \\(c{sl}{dl}(x)) \\((x as {d})) \\(y{dl})");')
  tk.append(f"f=t{sl}(x:{s}):i64{{\n"+"\n".join(body)+"\n  <0\n};")
mb=[]
for v in V:
  lit=f"({v})" if v.startswith("-") else v
  for s,sl,_,_ in T:
    mb.append(f"  t{sl}(({lit}) as {s});")
tk.append("f=main():i64{\n"+"\n".join(mb)+"\n  <0\n};")
open(sys.argv[1],"w").write("\n".join(tk)+"\n")
c=["#include <stdio.h>","#include <stdint.h>","#include <inttypes.h>","int main(void){"]
for v in V:
  for s,_,cs,_ in T:
    for d,_,cd,dsg in T:
      fmt="%\"PRId64\"" if dsg else "%\"PRIu64\""
      cast=f"(int64_t)({cd})({cs})(int64_t){v}LL" if dsg else f"(uint64_t)({cd})({cs})(int64_t){v}LL"
      c.append(f'  printf("{s}>{d} {fmt} {fmt} {fmt}\\n", {cast}, {cast}, {cast});')
c.append("  return 0;}")
open(sys.argv[2],"w").write("\n".join(c)+"\n")
PY

if "${TKC}" -o "${WORK}/m" "${WORK}/m.tk" >"${WORK}/m.log" 2>&1 && [ -x "${WORK}/m" ]; then
    expect "1-builds" "ok" "ok"
else
    expect "1-builds" "$(grep -o '"error_code":"E[0-9]*"' "${WORK}/m.log" | sort -u | tr '\n' ' ')" "ok"
fi
if ! "${CC}" -w -o "${WORK}/ref" "${WORK}/m.c" >"${WORK}/cc.log" 2>&1; then
    echo "FAIL: C reference did not build: $(head -c 300 "${WORK}/cc.log")"; exit 1
fi
"${WORK}/ref" >"${WORK}/ref.out"
if [ -x "${WORK}/m" ]; then
    "${WORK}/m" >"${WORK}/m.out" 2>&1
    expect "2-lines" "$(wc -l <"${WORK}/m.out" | tr -d ' ')" "1280"
    bad="$(diff "${WORK}/m.out" "${WORK}/ref.out" | grep -c '^<')"
    expect "2-matches-c" "${bad} differing lines" "0 differing lines"
    [ "${bad}" != 0 ] && diff "${WORK}/m.out" "${WORK}/ref.out" | head -10
else
    expect "2-matches-c" "not built" "0 differing lines"
fi

echo "C2040: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
