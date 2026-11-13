#!/usr/bin/env bash
# C1380_json_enc_record_typed.sh -- j.enc of a record encodes from the
# compiler's descriptor of its type, exactly the object j.print prints, and
# never walks the record as a C string (story 127.361).
#
# THE DEFECT.  127.353 typed j.print; the encode side still guessed.  The
# j.enc dispatch in src/llvm.c sent any argument that was not an array or a
# scalar to tk_json_enc_w, which casts it to `char *` and json_enc()s it.  A
# record is a bare malloc(nfields * 8) block (llvm.c struct_lit), so that was
# strlen over the record's slots: `j.enc(R{a:7;b:9})` returned "\u0007", and
# with no zero byte in any slot the read ran past the allocation (ASan
# heap-buffer-overflow).  An array of records went to tk_json_encarr_w and
# came back as element addresses.
# Now: a record (and an array holding records) encodes through
# tk_json_encval_w (json_glue.c) from the same descriptor json_type_desc()
# builds for tk_json_printval_w; a map, a sum or an opaque handle is E9002 at
# build, as j.print is.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan), records exactly nfields*8 bytes from malloc:
#      tk_json_encval_w over a flat record whose slots hold no zero byte, a
#      nested record, a record holding a [str] and an array of records, a
#      null handle -- exact text, no newline (sealed since 127.295: not freed).
#   2. toke, IR linked against ASan-built stdlib objects: j.enc of a record
#      literal, a let-bound record, a record returned by a function, a nested
#      record with every scalar kind, an array of records -- each line equal
#      to what j.print prints for the same value, and to the expected JSON;
#      scalars, str and a scalar array keep their existing encoders.
#   3. j.enc of a map, and of a sum value: --check passes, the build stops
#      with E9002 (no binary), no `fix` field.
#
# Negative control: TKC=<base tkc> C1380_STDLIB=<base src/stdlib>, base
# integrate/z-wave-1113 e1155b19: part 1 does not link (no tk_json_encval_w);
# part 2 dies at the first j.enc (ASan heap-buffer-overflow in json_enc /
# strlen, json_glue.c tk_json_enc_w); part 3 builds binaries -- 0 passed,
# 13 failed.  This branch: 17 passed.
#
# Story: 127.361
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1380_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1380_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1380: j.enc of a record is typed by the compiler, never guessed (127.361)"
echo "----------------------------------------------------------------------"

# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects json io -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; echo "C1380: ${PASS} passed, ${FAIL} failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > rec.c <<'C'
#include "tk_array.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_json_encval_w(int64_t v, int64_t desc);

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
static void enc(int64_t v, const char *d)
{
    char *s = (char *)(intptr_t)tk_json_encval_w(v, S(d));
    printf("[%s]\n", s);   /* brackets: the text carries no newline */
    /* 127.295: a sealed toke str since the twelfth S1 tranche -- never
     * freed (free() of a sealed string aborts); unfreed under the base's
     * malloc'd result only leaks, and leaks are off (detect_leaks=0). */
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* an ASan abort keeps every line so far */
    /* no slot holds a zero byte: a C-string walk would run off the end */
    enc(rec(2, (int64_t[]){ -1, -1 }), "{a:i,b:i}");
    double f = 1.5; int64_t fb; memcpy(&fb, &f, 8);
    int64_t in = rec(1, (int64_t[]){ -4 });
    enc(rec(4, (int64_t[]){ in, fb, 1, -1 }), "{in:{x:i},f:f,b:b,u:u}");
    enc(rec(2, (int64_t[]){ arr2(S("p"), S("q")),
                             arr2(rec(1, (int64_t[]){ 1 }), rec(1, (int64_t[]){ 2 })) }),
        "{ss:[s,ps:[{x:i}}");
    enc(0, "{x:i}");
    enc(rec(1, (int64_t[]){ 0 }), "{s:s}");
    puts("done");
    return 0;
}
C
if ! "${CC}" "${CF[@]}" -o rec rec.c "${LS[@]}" -lpthread -lm -lz > rec.cc 2>&1; then
    fail "C harness did not build: $(grep -m1 -E 'error|undefined' rec.cc | head -c 200)"
else
    ./rec > rec.out 2>&1; rc=$?
    [ "${rc}" -ne 0 ] && fail "rec: exited ${rc}: $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' rec.out | head -c 300)"
    n=1
    for want in '[{"a":-1,"b":-1}]' \
                '[{"in":{"x":-4},"f":1.5,"b":true,"u":18446744073709551615}]' \
                '[{"ss":["p","q"],"ps":[{"x":1},{"x":2}]}]' \
                '[null]' \
                '[{"s":null}]' \
                'done'; do
        got="$(sed -n "${n}p" rec.out)"
        if [ "${got}" = "${want}" ]; then pass "encval: ${want}"
        else fail "encval line ${n}: expected '${want}', got '${got}'"; fi
        n=$((n + 1))
    done
fi

# ── 2. toke, under ASan ──────────────────────────────────────────────────────
# Each record value is encoded and then printed, so line pairs must agree.
cat > p.tk <<'TOKE'
m=main;
i=io:std.io;
i=j:std.json;
t=P{x:i64};
t=R{a:i64;b:i64};
t=K{n:i32;u:u8;f:f64;g:f32;e:bool;s:str;xs:@i64;p:P;w:u64};
f=mk():R{<R{a:1;b:2}};
f=main():i64{
  io.println(j.enc(R{a:0-1;b:0-1}));
  j.print(R{a:0-1;b:0-1});
  let r=R{a:7;b:9};
  io.println(j.enc(r));
  j.print(r);
  io.println(j.enc(mk()));
  j.print(mk());
  let k=K{n:(0-3) as i32;u:200 as u8;f:1.5;g:2.5 as f32;e:true;s:"q\"x";xs:@(1;2);p:P{x:4};w:18446744073709551615 as u64};
  io.println(j.enc(k));
  j.print(k);
  let ps=@(P{x:1};P{x:2});
  io.println(j.enc(ps));
  j.print(ps);
  io.println(j.enc("hi"));
  io.println(j.enc(@(1;2;3)));
  io.println(j.enc(4294967296));
  io.println(j.enc(false));
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
    for want in '{"a":-1,"b":-1}' \
                '{"a":7,"b":9}' \
                '{"a":1,"b":2}' \
                '{"n":-3,"u":200,"f":1.5,"g":2.5,"e":true,"s":"q\"x","xs":[1,2],"p":{"x":4},"w":18446744073709551615}' \
                '[{"x":1},{"x":2}]'; do
        enc="$(sed -n "${n}p" p.out)"; prn="$(sed -n "$((n + 1))p" p.out)"
        if [ "${rc}" -eq 0 ] && [ "${enc}" = "${want}" ] && [ "${prn}" = "${want}" ]; then
            pass "toke j.enc == j.print: ${want}"
        else
            fail "toke line ${n}: expected '${want}' twice, got enc '${enc}', print '${prn}'"
        fi
        n=$((n + 2))
    done
    for want in '"hi"' '[1,2,3]' '4294967296' 'false'; do
        got="$(sed -n "${n}p" p.out)"
        if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "toke j.enc unchanged: ${want}"
        else fail "toke j.enc line ${n}: expected '${want}', got '${got}'"; fi
        n=$((n + 1))
    done
fi

# ── 3. a map / a sum has no JSON encoder: E9002, not a guess ─────────────────
cat > m.tk <<'TOKE'
m=main;
i=io:std.io;
i=j:std.json;
f=main():i64{
  let mp=@("a":1);
  io.println(j.enc(mp));
  <0
};
TOKE
cat > s.tk <<'TOKE'
m=main;
i=io:std.io;
i=j:std.json;
t=$sh{$c:i64;$q:i64};
f=main():i64{
  let v=$sh{$c:2};
  io.println(j.enc(v));
  <0
};
TOKE
for f in m s; do
    "${TKC}" --check "${f}.tk" > "${f}.chk" 2>&1; crc=$?
    rm -f "${f}b"
    "${TKC}" "${f}.tk" -o "${f}b" > "${f}.log" 2>&1; brc=$?
    if [ "${crc}" -eq 0 ] && [ "${brc}" -ne 0 ] && [ ! -e "${f}b" ] \
       && grep -q '"error_code":"E9002"' "${f}.log" && ! grep -q '"fix"' "${f}.log"; then
        pass "j.enc(${f}): --check rc 0, build E9002, no binary, no fix"
    else
        fail "j.enc(${f}): check rc ${crc}, build rc ${brc}, binary $([ -e "${f}b" ] && echo yes || echo no): $(head -c 300 "${f}.log")"
    fi
done

echo "----------------------------------------------------------------------"
echo "C1380: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
