#!/usr/bin/env bash
# C1350_json_print_typed_not_guessed.sh -- j.print picks its printer from the
# argument's static type and never reads a header word from a value that is
# not statically an array (story 127.353).
#
# THE DEFECT.  tk_json_print (src/stdlib/tk_runtime.c) guessed: any i64
# outside +-1e9 was a pointer, printed as a string when its first byte was
# printable ASCII and otherwise as an int array whose length it read from
# val[-1].  The codegen dispatch in src/llvm.c had taken most types away from
# it (127.43/44/45), but what was left still guessed:
#   - a record went to tk_json_print_arr, which read rec[-1].  A record is a
#     bare malloc(nfields * 8) with no header (llvm.c struct_lit), so that is
#     8 bytes before the allocation: `j.print(R{a:7;b:9})` printed `[]`, and
#     under ASan it is a heap-buffer-overflow;
#   - an array of records printed its element addresses;
#   - an f32 (its f64 bits as an i64) and an i32 past 1e9 reached the
#     generic tk_json_print, which dereferenced them (exit 139);
#   - a map went to tk_json_print_arr too.
# Now: a record (and an array holding records) prints as a JSON object from a
# descriptor of its checker type (tk_json_printval_w, json_glue.c); f32 and
# narrow integers go to the typed printers; a map, a sum, a function or a
# pointer is an E9002 at build, never a guess; and tk_json_print itself prints
# its i64 and reads no memory.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan), records are EXACTLY nfields*8 bytes from malloc, so a
#      read of rec[-1] lands in ASan's redzone:
#        - tk_json_printval_w over a flat record, a nested record, a record
#          holding a [str] and an array of records, a null handle;
#        - tk_json_print on a record pointer and on a str pointer prints the
#          number and touches neither.
#   2. toke, the program's IR linked against ASan-built stdlib objects:
#      j.print of a record, a record returned by a function, a nested record
#      with every scalar kind, an array of records, a str, an array, an i64,
#      a bool, an f64, an f32, an i32 past 1e9 and a u32 past 2^31 -- exact
#      output, no sanitizer report.
#   3. j.print of a map: --check passes, the build stops with E9002 (no
#      binary), and no `fix` field.
#
# Negative control: TKC=<main tkc> C1350_STDLIB=<main src/stdlib>, main
# aace8185: 0 passed, 15 failed -- part 1 does not link (no
# tk_json_printval_w); part 2 dies at the first record (ASan heap-buffer-
# overflow in tk_json_print_arr, tk_runtime.c:364, "8 bytes before 16-byte
# region"); part 3 builds a binary.  This branch: 21 passed.
#
# Story: 127.353
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1350_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1350_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1350: j.print is typed by the compiler, never guessed (127.353)"
echo "----------------------------------------------------------------------"

# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects json io -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "C1350: ${PASS} passed, ${FAIL} failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > rec.c <<'C'
#include "tk_array.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void tk_json_printval_w(int64_t v, int64_t desc);
void tk_json_print(int64_t val);

#define S(x) ((int64_t)(intptr_t)(x))
/* A record exactly as the compiler makes one: nfields i64 slots, no header. */
static int64_t rec(int n, const int64_t *v)
{
    int64_t *b = malloc((size_t)n * sizeof(int64_t));
    memcpy(b, v, (size_t)n * sizeof(int64_t));
    return S(b);
}
static int64_t arr2(int64_t a, int64_t b)
{
    int64_t h = tk_arr_alloc(2, 2);
    ((int64_t *)(intptr_t)h)[0] = a; ((int64_t *)(intptr_t)h)[1] = b;
    return h;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* an ASan abort keeps every line so far */
    int64_t r = rec(2, (int64_t[]){ 7, S("hi") });
    /* the generic printer first: it must print the number, not read r[-1] */
    tk_json_print(r);
    printf("want %lld\n", (long long)r);
    tk_json_print(S("hello"));
    printf("want %lld\n", (long long)S("hello"));
    tk_json_printval_w(r, S("{a:i,b:s}"));
    double f = 1.5; int64_t fb; memcpy(&fb, &f, 8);
    int64_t in = rec(1, (int64_t[]){ -4 });
    int64_t out = rec(4, (int64_t[]){ in, fb, 1, -1 });
    tk_json_printval_w(out, S("{in:{x:i},f:f,b:b,u:u}"));
    int64_t ws = rec(2, (int64_t[]){ arr2(S("p"), S("q")),
                                      arr2(rec(1, (int64_t[]){ 1 }), rec(1, (int64_t[]){ 2 })) });
    tk_json_printval_w(ws, S("{ss:[s,ps:[{x:i}}"));
    tk_json_printval_w(0, S("{x:i}"));
    tk_json_printval_w(rec(1, (int64_t[]){ 0 }), S("{s:s}"));
    puts("done");
    return 0;
}
C
if ! "${CC}" "${CF[@]}" -o rec rec.c "${LS[@]}" -lpthread -lm -lz > rec.cc 2>&1; then
    fail "C harness did not build: $(grep -m1 -E 'error|undefined' rec.cc | head -c 200)"
else
    ./rec > rec.out 2>&1; rc=$?
    [ "${rc}" -ne 0 ] && fail "rec: exited ${rc}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' rec.out | head -c 300)"
    # lines 1-4: each generic print equals its "want" line
    l1="$(sed -n 1p rec.out)"; w1="$(sed -n 2p rec.out)"
    l2="$(sed -n 3p rec.out)"; w2="$(sed -n 4p rec.out)"
    if [ -n "${l1}" ] && [ "want ${l1}" = "${w1}" ]; then pass "tk_json_print(record) prints the i64, reads nothing"
    else fail "tk_json_print(record): got '${l1}', ${w1}"; fi
    if [ -n "${l2}" ] && [ "want ${l2}" = "${w2}" ]; then pass "tk_json_print(str pointer) prints the i64, reads nothing"
    else fail "tk_json_print(str pointer): got '${l2}', ${w2}"; fi
    n=5
    for want in '{"a":7,"b":"hi"}' \
                '{"in":{"x":-4},"f":1.5,"b":true,"u":18446744073709551615}' \
                '{"ss":["p","q"],"ps":[{"x":1},{"x":2}]}' \
                'null' \
                '{"s":null}' \
                'done'; do
        got="$(sed -n "${n}p" rec.out)"
        if [ "${got}" = "${want}" ]; then pass "printval: ${want}"
        else fail "printval line ${n}: expected '${want}', got '${got}'"; fi
        n=$((n + 1))
    done
fi

# ── 2. toke, under ASan ──────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=main;
i=j:std.json;
t=P{x:i64};
t=R{a:i64;b:i64};
t=K{n:i32;u:u8;f:f64;g:f32;e:bool;s:str;xs:@i64;p:P;w:u64};
f=mk():R{<R{a:1;b:2}};
f=main():i64{
  let r=R{a:7;b:9};
  j.print(r);
  j.print(mk());
  let k=K{n:0-3;u:200 as u8;f:1.5;g:2.5 as f32;e:true;s:"q\"x";xs:@(1;2);p:P{x:4};w:18446744073709551615 as u64};
  j.print(k);
  j.print(@(P{x:1};P{x:2}));
  j.print("hi");
  j.print(@(1;2;3));
  j.print(4294967296);
  j.print(false);
  j.print(0.25);
  let g=100000000000.0 as f32;
  j.print(g);
  let big:i32=2000000000;
  j.print(big);
  let u:u32=4000000000;
  j.print(u);
  <0
};
TOKE
if ! "${TKC}" --emit-llvm p.tk -o p.ll > p.log 2>&1; then
    fail "p.tk did not compile: $(head -c 300 p.log)"
elif ! "${CC}" ${SAN} -g -o p p.ll "${LS[@]}" -lpthread -lm -lz > p.cc 2>&1; then
    fail "p.ll did not link: $(grep -m1 -E 'error|undefined' p.cc | head -c 200)"
else
    ./p > p.out 2>&1; rc=$?
    [ "${rc}" -ne 0 ] && fail "p: exited ${rc}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|SUMMARY' p.out | head -c 300)"
    n=1
    for want in '{"a":7,"b":9}' \
                '{"a":1,"b":2}' \
                '{"n":-3,"u":200,"f":1.5,"g":2.5,"e":true,"s":"q\"x","xs":[1,2],"p":{"x":4},"w":18446744073709551615}' \
                '[{"x":1},{"x":2}]' \
                '"hi"' \
                '[1,2,3]' \
                '4294967296' \
                'false' \
                '0.25' \
                '1e+11' \
                '2000000000' \
                '4000000000'; do
        got="$(sed -n "${n}p" p.out)"
        if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "toke j.print: ${want}"
        else fail "toke j.print line ${n}: expected '${want}', got '${got}'"; fi
        n=$((n + 1))
    done
fi

# ── 3. a map has no JSON printer: E9002, not a guess ─────────────────────────
cat > m.tk <<'TOKE'
m=main;
i=j:std.json;
f=main():i64{
  let mp=@("a":1);
  j.print(mp);
  <0
};
TOKE
"${TKC}" --check m.tk > m.chk 2>&1; crc=$?
rm -f mb
"${TKC}" m.tk -o mb > m.log 2>&1; brc=$?
if [ "${crc}" -eq 0 ] && [ "${brc}" -ne 0 ] && [ ! -e mb ] \
   && grep -q '"error_code":"E9002"' m.log && ! grep -q '"fix"' m.log; then
    pass "j.print(map): --check rc 0, build E9002, no binary, no fix"
else
    fail "j.print(map): check rc ${crc}, build rc ${brc}, binary $([ -e mb ] && echo yes || echo no): $(head -c 300 m.log)"
fi

echo "----------------------------------------------------------------------"
echo "C1350: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
