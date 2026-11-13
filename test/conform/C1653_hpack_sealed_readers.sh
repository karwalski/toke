#!/usr/bin/env bash
# C1653_hpack_sealed_readers.sh -- http2.c's HPACK encoder reads header names
# and values with tk_strlen (story 127.295, S1 step 5 tranche 14;
# runtime-abi §3.1).
#
# http2.c hands toke no string: its four strlen readers are the HPACK
# encoder's name/value lengths (hpack_encode's two literal forms and
# hpack_insert's RFC 7541 §4.1 entry size).  They are tk_strlen now, which
# reads a sealed header's length from its header and an unsealed one with
# strlen.  The response headers a toke handler sets reach them sealed.
#
# WHAT IS ASSERTED (ASan+UBSan, TK_STR_VERIFY=1, so every trusted length
# read asserts header == strlen):
#   1. hpack_encode of the same three headers -- one fully indexed
#      (:status 200), one with an indexed name (content-type), one new
#      (x-c1653) -- gives byte-identical blocks for sealed and for plain
#      malloc'd names and values, each into a fresh table;
#   2. the second block of each table is identical too (it hits the entries
#      hpack_insert added, whose size it computed from those lengths);
#   3. hpack_decode of the sealed block gives back the three headers.
#
# This is a readers-only test: it passes on base e810734c as well (an
# unsealed reader was always correct); it guards the conversion against a
# length read that disagrees with strlen.
# Measured: this branch 3 passed; base e810734c 3 passed.
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1653_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1653_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1653: http2.c HPACK encoder reads sealed headers with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------"

cat > hp.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "http2.h"
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static const char *N[3] = { ":status", "content-type", "x-c1653" };
static const char *V[3] = { "200", "text/plain; charset=utf-8", "sealed-value" };
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *sn[3], *sv[3], *un[3], *uv[3];
    int sealed = 1;
    for (int i = 0; i < 3; i++) {
        sn[i] = tks_strdup(N[i]); sv[i] = tks_strdup(V[i]);
        un[i] = strdup(N[i]);     uv[i] = strdup(V[i]);
        sealed &= tks_hdrlen(sn[i]) >= 0 && tks_hdrlen(sv[i]) >= 0;
    }
    if (!sealed) printf("        (the sealed copies have no header)\n");
    HpackTable ts, tu;
    hpack_table_init(&ts, 4096);
    hpack_table_init(&tu, 4096);
    uint8_t bs[2][512], bu[2][512];
    int ls[2], lu[2];
    for (int k = 0; k < 2; k++) {
        ls[k] = hpack_encode(&ts, sn, sv, 3, bs[k], sizeof bs[k]);
        lu[k] = hpack_encode(&tu, un, uv, 3, bu[k], sizeof bu[k]);
    }
    CHECK(sealed && ls[0] > 0 && ls[0] == lu[0] && !memcmp(bs[0], bu[0], (size_t)ls[0]),
          "first block: sealed and unsealed headers encode identically");
    CHECK(ls[1] > 0 && ls[1] == lu[1] && !memcmp(bs[1], bu[1], (size_t)ls[1]) && ls[1] < ls[0],
          "second block: identical, and shorter (the inserted entries are indexed)");
    HpackTable td;
    hpack_table_init(&td, 4096);
    char **dn = NULL, **dv = NULL;
    int n = hpack_decode(&td, bs[0], (size_t)ls[0], &dn, &dv);
    int ok = n == 3;
    for (int i = 0; ok && i < 3; i++) ok = !strcmp(dn[i], N[i]) && !strcmp(dv[i], V[i]);
    CHECK(ok, "hpack_decode of the sealed block gives back the three headers");
    for (int i = 0; i < n && n > 0; i++) { free(dn[i]); free(dv[i]); }
    free(dn); free(dv);
    hpack_table_free(&ts); hpack_table_free(&tu); hpack_table_free(&td);
    for (int i = 0; i < 3; i++) { free((void *)un[i]); free((void *)uv[i]); }
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (http2.c is part of std.http).  Built
# without TK_HAVE_OPENSSL, tls.c is its stub, so no -lssl is needed.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects http -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o hp hp.c "${LS[@]}" -lz -lpthread -lm > hp.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' hp.cc | head -6
else
    TK_STR_VERIFY=1 ./hp > hp.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' hp.out | head -20
    PASS=$((PASS + $(grep -c '^  PASS' hp.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' hp.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' hp.out; then
        fail "C harness exited ${rc}"; tail -8 hp.out | sed 's/^/        /'
    fi
fi

echo "--------------------------------------------------------------------------"
echo "C1653: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
