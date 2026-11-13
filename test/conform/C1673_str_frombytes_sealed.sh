#!/usr/bin/env bash
# C1673_str_frombytes_sealed.sh -- str.frombytes hands toke a sealed string
# (story 127.295, S1 step 5 tranche 16; runtime-abi §3.1).
#
# str.frombytes was the one str producer every tranche left out: its wrapper
# returned tk_bytes_unpack's malloc'd buffer, after freeing str_from_bytes's
# validated copy.  It now hands toke a sealed copy of the buffer, measured as
# a C string -- so a [byte] holding a 0 gives the str every reader already
# saw (the bytes before the 0), with a header equal to strlen -- and frees
# the buffer.  An invalid UTF-8 input still raises EncodingErr and still
# carries the bytes on the value channel (127.241), now sealed too.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1): tk_str_frombytes_w of
#      "hello" is a sealed "hello"; of no bytes a sealed ""; of "ab\0cd" a
#      sealed "ab" (header 2); of 0xff (invalid UTF-8) a sealed "\xff" with
#      tk_current_error set; str.from_bytes (the alias) the same as
#      str.frombytes.
#   2. toke, under TK_STR_VERIFY=1: str.frombytes(str.bytes(s)) of a built
#      string, its .len and charat, and the $err arm on invalid UTF-8.
#
# Negative control: C1673_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base 684b452c's src/stdlib + stdlib: 1 passed, 4 failed -- every C
#   case reports "not sealed"; the toke case passes (an unsealed string
#   read with strlen was always correct).
# Measured: this branch 5 passed (4 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1673_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1673_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1673: str.frombytes results sealed (127.295)"
echo "---------------------------------------------"

cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "bytes_rt.h"
int64_t tk_str_frombytes_w(int64_t b);
int64_t tk_str_from_bytes_w(int64_t b);
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define P(v) ((const char *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.40s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || strcmp(s, want) != 0)
        printf("        (got '%.40s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && strcmp(s, want) == 0;
}
static int64_t pack(const char *s, size_t n) { return tk_bytes_pack((const uint8_t *)s, n); }

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    tk_current_error = 1;
    CHECK(sealed_eq(P(tk_str_frombytes_w(pack("hello", 5))), "hello") && tk_current_error == 0
          && sealed_eq(P(tk_str_frombytes_w(pack("", 0))), ""),
          "str.frombytes: a sealed copy; of no bytes a sealed \"\"");
    const char *z = P(tk_str_frombytes_w(pack("ab\0cd", 5)));
    CHECK(sealed_eq(z, "ab") && tks_hdrlen(z) == 2,
          "str.frombytes of bytes holding a 0: sealed with the C-string length");
    tk_current_error = 0;
    CHECK(sealed_eq(P(tk_str_frombytes_w(pack("\xff", 1))), "\xff") && tk_current_error != 0,
          "str.frombytes of invalid UTF-8: EncodingErr raised, the bytes sealed");
    CHECK(sealed_eq(P(tk_str_from_bytes_w(pack("alias", 5))), "alias"),
          "str.from_bytes (alias): sealed");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects str -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && grep -a -q -E 'ERROR: AddressSanitizer|runtime error' seal.out; then
        fail "C harness stopped: $(grep -a -m1 -o -E 'AddressSanitizer: [a-z-]+|runtime error: .{0,60}' seal.out)"
    elif [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let b=str.builder();
  str.add(b;"tok,");
  str.add(b;"en");
  let t=mt str.frombytes(str.bytes(str.build(b))){$ok:v v;$err:e "?"};
  io.println("t=\(t) \(t.len) \(t.charat(3))");
  let bad=mt str.frombytes(@(255 as byte)){$ok:v "ok";$err:e "err"};
  io.println("bad=\(bad)");
  <0
};
TOKE
WANT='t=tok,en 6 44
bad=err'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: str.frombytes, .len, charat and the \$err arm under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "---------------------------------------------"
echo "C1673: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
