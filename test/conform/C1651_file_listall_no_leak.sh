#!/usr/bin/env bash
# C1651_file_listall_no_leak.sh -- file.listall frees its pointer block
# (story 127.415).
#
# tk_file_listall_w handed file_listall's malloc'd pointer block to
# file_strarray with free_data=0, so the block (file_listall's thread-local
# accumulator, replaced by the next call) leaked on every call: 64 pointers,
# 512 bytes on a 64-bit target.  file.list, readlines and listglob free theirs.
# Since 127.295 tranche 13 every element is a sealed copy, so the glue now
# frees the block too.
#
# HOW THE LEAK IS MEASURED: ASan's allocator byte count
# (__sanitizer_get_current_allocated_bytes) over N = 200 calls, every result
# kept reachable.  LeakSanitizer (detect_leaks=1) would be the direct check,
# but Darwin's ASan does not support it ("detect_leaks is not supported on
# this platform"), and the byte count works on both.  The yardstick is
# file.list over the same flat directory of four files: it makes the same
# toke array of four sealed strings (sealed strings live in the str reserve,
# not the malloc heap) and frees its block, so the two growths must match.
#
# WHAT IS ASSERTED (ASan+UBSan, TK_STR_VERIFY=1):
#   1. file.listall and file.list return the same four names;
#   2. the heap growth of 200 file.listall calls exceeds that of 200 file.list
#      calls by less than 200 x 64 bytes (the base: 200 x 512 = 102,400).
#
# Negative control: C1651_STDLIB=<dir> uses another tree's src/stdlib.
#   Base e810734c: 1 passed, 1 failed (102,400 bytes more than file.list).
# Measured: this branch 2 passed (0 bytes more).
#
# Story: 127.415
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1651_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
# 127.439: clang: the harness reads <sanitizer/allocator_interface.h>, which
# compiler-rt ships and gcc's libsanitizer does not (Linux cc is gcc).
CC="${CC:-clang}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1651_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1651: file.listall frees its pointer block (127.415)"
echo "--------------------------------------------------------"

mkdir -p d
for n in w x y z; do printf '%s' "$n" > "d/$n"; done

cat > leak.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sanitizer/allocator_interface.h>
#include "file.h"
#include "capabilities.h"
#include "tk_runtime.h"
int64_t tk_file_list_w(int64_t dir);
int64_t tk_file_listall_w(int64_t dir);
#define N 200
static int64_t keep[2][N];          /* every result stays reachable */
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int cmp(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }
static int names(int64_t a, const char **out)
{
    if (!a || ((int64_t *)(intptr_t)a)[-1] != 4) return 0;
    for (int i = 0; i < 4; i++) out[i] = (const char *)(intptr_t)((int64_t *)(intptr_t)a)[i];
    qsort(out, 4, sizeof out[0], cmp);
    return 1;
}
static long long growth(int64_t (*fn)(int64_t), int64_t *slot)
{
    for (int i = 0; i < 5; i++) (void)fn((int64_t)(intptr_t)"d");   /* warm up */
    size_t before = __sanitizer_get_current_allocated_bytes();
    for (int i = 0; i < N; i++) slot[i] = fn((int64_t)(intptr_t)"d");
    return (long long)__sanitizer_get_current_allocated_bytes() - (long long)before;
}
int main(void)
{
    char a0[] = "leak", a1[] = "--allow-read";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);
    setvbuf(stdout, NULL, _IONBF, 0);
    const char *l[4], *a[4];
    int ok = names(tk_file_list_w((int64_t)(intptr_t)"d"), l)
          && names(tk_file_listall_w((int64_t)(intptr_t)"d"), a);
    for (int i = 0; ok && i < 4; i++) ok = strcmp(l[i], a[i]) == 0;
    CHECK(ok, "file.listall and file.list return the same four names");
    long long gl = growth(tk_file_list_w, keep[0]);
    long long ga = growth(tk_file_listall_w, keep[1]);
    printf("        (heap growth over %d calls: file.list %lld, file.listall %lld, "
           "difference %lld bytes)\n", N, gl, ga, ga - gl);
    CHECK(ga - gl < (long long)N * 64,
          "200 file.listall calls grow the heap no more than 200 file.list calls");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects file -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o leak leak.c "${LS[@]}" -lz -lpthread -lm > leak.cc 2>&1; then
    fail "harness compile"; head -20 leak.cc
else
    TK_STR_VERIFY=1 ./leak > leak.out 2> leak.err
    rc=$?
    cat leak.out
    p=$(grep -c '^  PASS:' leak.out); f=$(grep -c '^  FAIL:' leak.out)
    PASS=$((PASS + p)); FAIL=$((FAIL + f))
    if [ "${rc}" -ne 0 ] && [ "${f}" -eq 0 ]; then
        fail "harness exited ${rc} (sanitizer?)"; head -30 leak.err
    fi
fi

echo "--------------------------------------------------------"
echo "C1651: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
