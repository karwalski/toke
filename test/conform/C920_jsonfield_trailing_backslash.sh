#!/usr/bin/env bash
# C920_jsonfield_trailing_backslash.sh — encoding.jsonfield stops at the end
# of its input when the value ends in a backslash, and returns a sealed
# string (story 127.317).
#
# THE DEFECT.  tk_encoding_jsonfield_w (src/stdlib/encoding_glue.c) scanned
#   the value with `if (*vend=='\\') vend++; vend++;` inside
#   `while (*vend && *vend!='"')`.  A backslash that is the input's last byte
#   (`{"k":"ab\` with no closing quote) stepped over the NUL, and the loop
#   kept reading past the string -- an out-of-bounds read, and the over-read
#   bytes (a 0 among them) were copied into the result.  Z96 (127.295) left
#   the result malloc'd rather than sealed until this was fixed.
#
# THE FIX.  The escape skip only steps when a byte follows the backslash, so
#   the value ends at the NUL with the backslash kept; the scanned range can
#   then hold no 0 byte, and the result is a sealed string (tks_dup).
#
# WHAT IS ASSERTED.
#   Part 1 (C driver, encoding_glue.c under ASan+UBSan): the values read, a
#     trailing backslash ends the value with no over-read, and every result
#     carries a length header equal to strlen.
#   Part 2 (toke program, stdlib compiled through an ASan TKC_CC wrapper):
#     the same reads end to end, run under TK_STR_VERIFY=1.  encoding.jsonfield
#     returns i64 in encoding.tki (gen "abi"), so the program sees only that
#     each call ran and returned non-zero; the bytes are Part 1's job.
#
# Negative control: C920_STDLIB=<repo>/src/stdlib builds against another
# tree.  Measured 2026-11-09 against integrate/z-wave-1108 f397c9a0: Part 1's
# driver dies under ASan (global-buffer-overflow in tk_encoding_jsonfield_w)
# and Part 2's program does too.
#
# Story: 127.317
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C920_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c920_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C920: encoding.jsonfield stops at a trailing backslash; result sealed (127.317)"
echo "---------------------------------------------------------------------------"
SANF="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

# ── Part 1: the C glue ───────────────────────────────────────────────────
cat > t.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
/* The error slot (tk_current_error, tk_err_box) comes from tk_runtime.c in the link set. */
int64_t tk_encoding_jsonfield_w(int64_t, int64_t);
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
/* Copy into an exact-size malloc block so ASan sees the end of the input. */
static const char *field(const char *doc, const char *key) {
    size_t n = strlen(doc);
    char *d = malloc(n + 1);
    memcpy(d, doc, n + 1);
    const char *r = (const char *)(intptr_t)tk_encoding_jsonfield_w(
        (int64_t)(intptr_t)d, (int64_t)(intptr_t)key);
    free(d);
    return r;
}
static int is(const char *r, const char *want) {
    return r && strcmp(r, want) == 0 && tks_hdrlen(r) == (int64_t)strlen(want);
}
int main(void) {
    CHECK(is(field("{\"k\":\"hello\"}", "k"), "hello"), "plain value, sealed");
    CHECK(is(field("{\"a\":1,\"k\": \"v w\"}", "k"), "v w"), "spaced value after another member");
    CHECK(is(field("{\"k\":\"x\\\"y\",\"z\":1}", "k"), "x\\\"y"), "escaped quote kept as written");
    CHECK(is(field("{\"k\":\"\"}", "k"), ""), "empty value, sealed");
    CHECK(is(field("{\"k\":\"ab\\", "k"), "ab\\"), "trailing backslash ends the value (no over-read)");
    CHECK(is(field("{\"k\":\"\\", "k"), "\\"), "lone trailing backslash");
    CHECK(is(field("{\"k\":\"ab\\\\", "k"), "ab\\\\"), "escaped backslash, then end of input");
    CHECK(is(field("{\"k\":\"open", "k"), "open"), "unterminated value runs to the end");
    CHECK(field("{\"k\":1}", "k") == NULL && field("{\"j\":\"v\"}", "k") == NULL,
          "non-string value / missing key is 0");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SANF} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" --objects encoding -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set: see stderr above"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o t t.c "${LS[@]}" -lpthread > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    ./t > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing:"
        grep -a -m1 'ERROR: AddressSanitizer\|runtime error' t.out | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

# ── Part 2: end to end ───────────────────────────────────────────────────
CLANG="$(command -v clang || true)"
if [ -z "${CLANG}" ]; then
    echo "  FAIL: clang not on PATH for the ASan TKC_CC wrapper"; FAIL=$((FAIL + 1))
else
    printf '#!/bin/sh\nexec "%s" "$@" %s -g\n' "${CLANG}" "${SANF}" > asancc
    chmod +x asancc
    cat > d.tk <<'TOKE'
m=d;i=io:std.io;i=enc:std.encoding;
f=main():i64{
  let a=enc.jsonfield("{\"k\":\"ab\\";"k");
  let b=enc.jsonfield("{\"k\":\"\\";"k");
  let c=enc.jsonfield("{\"k\":\"x\\\"y\",\"z\":1}";"k");
  let d=enc.jsonfield("{\"k\":2}";"k");
  io.println("a=\(a>0) b=\(b>0) c=\(c>0) d=\(d==0)");
  <0
};
TOKE
    want='a=true b=true c=true d=true'
    if ! TKC_CC="${WORK}/asancc" TKC_STDLIB_DIR="${S}" "${TKC}" --out d d.tk > d.log 2>&1; then
        echo "  FAIL: d.tk did not compile"; head -4 d.log | sed 's/^/        /'; FAIL=$((FAIL + 1))
    else
        got="$(TK_STR_VERIFY=1 ./d 2>&1)"; rc=$?
        if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
            echo "  PASS: e2e ${want} (ASan, TK_STR_VERIFY=1)"; PASS=$((PASS + 1))
        else
            echo "  FAIL: e2e rc=${rc}, expected '${want}', got:"
            printf '%s\n' "${got}" | grep -a -m3 'ERROR\|runtime error\|=' | sed 's/^/        /'
            FAIL=$((FAIL + 1))
        fi
    fi
fi

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
