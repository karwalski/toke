#!/usr/bin/env bash
# C211_extern_own_c_symbol.sh — a user extern may not give a C function the
# compiler itself calls (malloc; the loop guard's fprintf and exit) a
# different signature: E4060 at the extern, no fix.
#
# THE DEFECT (127.219 item 2), on main at c8575da.  One symbol gets one
# `declare`, and the body's (the user's) won the dedup, so the compiler's own
# calls were emitted against the user's signature:
#   * `f=malloc(a:i64;b:i64):i64;` plus any array literal built and ran, the
#     array block allocated through a two-parameter malloc called with one;
#   * `f=exit(n:i64):i64;` or `f=fprintf(a:i64;b:i64):i64;` under
#     --max-iters failed inside clang ("invalid redefinition of function
#     'exit'") with no toke diagnostic -- and so did an extern that REPEATS
#     exit's own declaration, because the loop guard declared it a second time.
#
# PREMISE CORRECTED: the row also named printf, puts and strcmp.  Those are in
# the compiler's declaration table but nothing the compiler emits calls them,
# so a user extern of another signature is the only declaration and the only
# caller.  They stay accepted (cases 6, 7).
#
# The build exits 2, as every diagnostic raised during code generation does
# (main.c maps a failed emit_llvm_ir to EINTERNAL); --check does not see it.
#
# Negative control: TKC=<main's toke> — cases 1-5 fail (no E4060; 3-5 show
# clang's "invalid redefinition"), 6-7 pass on both.
#
# Story: 127.219 (item 2)

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
cd "${WORK}" || exit 1
# error codes (warnings dropped), and whether any E4060 line carries a fix
errs() { awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
fixes() { awk '/"error_code":"E4060"/ && /"fix":/{c++} END{print c+0}'; }

echo "C211: extern vs the compiler's own C symbols (127.219 item 2)"

build() {  # NAME FLAGS... ; source in NAME.tk -> "<rc>|<errors>|<fixes>"
    local n="$1"; shift
    "${TKC}" "$@" "${n}.tk" -o "${n}.bin" > "${n}.out" 2>&1; local rc=$?
    echo "${rc}|$(errs < "${n}.out")|$(fixes < "${n}.out")"
}

printf 'm=main;\nf=malloc(a:i64;b:i64):i64;\nf=main():i64{let a=@(1;2;3);<malloc(1;2)};\n' > c1.tk
expect "1 malloc with two params + an array literal" "$(build c1)" "2|E4060 |0"

printf 'm=main;\nf=malloc(n:i64):i64;\nf=main():i64{let a=@(1;2;3);<a.len as i64};\n' > c2.tk
expect "2 malloc returning i64" "$(build c2)" "2|E4060 |0"

printf 'm=main;\nf=exit(n:i64):i64;\nf=main():i64{lp(let i=0;i<3;i=i+1){};<0};\n' > c3.tk
expect "3 exit(i64):i64 under --max-iters" "$(build c3 --max-iters=100)" "2|E4060 |0"

printf 'm=main;\nf=exit(n:i32):void;\nf=main():i64{lp(let i=0;i<3;i=i+1){};<0};\n' > c4.tk
r="$(build c4 --max-iters=100)"
if [ "${r%%|*}" = "0" ]; then ./c4.bin; r="${r}|run=$?"; fi
expect "4 exit with the compiler's own signature, --max-iters: builds, runs" "${r}" "0||0|run=0"

printf 'm=main;\nf=fprintf(a:i64;b:i64):i64;\nf=main():i64{lp(let i=0;i<3;i=i+1){};<0};\n' > c5.tk
expect "5 fprintf(i64;i64) under --max-iters" "$(build c5 --max-iters=100)" "2|E4060 |0"

printf 'm=main;\nf=puts(s:$str):i32;\nf=main():i64{let r=puts("hi");<0};\n' > c6.tk
r="$(build c6)"
if [ "${r%%|*}" = "0" ]; then r="${r}|$(./c6.bin)"; fi
expect "6 puts extern (never called by the compiler) still accepted" "${r}" "0||0|hi"

printf 'm=main;\nf=strcmp(a:$str;b:$str):i32;\nf=main():i64{<strcmp("a";"a") as i64};\n' > c7.tk
r="$(build c7)"
if [ "${r%%|*}" = "0" ]; then ./c7.bin; r="${r}|run=$?"; fi
expect "7 strcmp extern still accepted" "${r}" "0||0|run=0"

echo "C211: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
