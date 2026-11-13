#!/usr/bin/env bash
# C1672_csv_glue_no_leak.sh -- csv.parse, csv.reader / csv.next and
# csv.flush free what they allocate for themselves (story 127.422).
#
# THE DEFECT.  src/stdlib/csv_glue.c leaked on every call: tk_csv_parse_w
# and tk_csv_reader_w kept tk_bytes_unpack's malloc'd copy of the whole input
# (each then made a second copy), tk_csv_parse_w and tk_csv_next_w kept each
# row's StrArray pointer block after copying its pointers into a toke array,
# and tk_csv_flush_w kept csv_writer_flush's malloc'd text after packing it
# into the [byte] it returns.  None of these is a value toke can reach, so
# nothing else could ever free them.
#
# WHAT IS ASSERTED.  A C driver (no sanitizer: it reads the allocator's own
# in-use byte count -- malloc_zone_statistics on Darwin, mallinfo2 on glibc;
# elsewhere the test is a CONFORM-SKIP) runs each wrapper 20000 times on a
# small document, freeing everything toke would own (the arrays, the row
# records, any field that is not a sealed string -- sealed strings live in
# str.c's own mapping, not the malloc heap), and closing each reader and
# writer.  Each loop's growth must stay under half a block per call on
# Darwin (blocks_in_use; the allocator's caches move it by a few thousand)
# or 8 bytes per call on glibc (uordblks): every leak was at least one block
# (16 bytes) per call.
#
# Negative control: C1672_STDLIB=<dir> uses another tree's src/stdlib.
#   Base 684b452c's src/stdlib: 0 passed, 3 failed (Darwin, over 20000
#   calls: parse grew 103402 blocks, reader + next 100539, flush 19626 --
#   five, five and one leaked block per call).
# Measured: this branch 3 passed (under 5000 blocks each, run to run: the
# allocator's caches, not a count that grows with the calls).
#
# Story: 127.422
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1672_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1672_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1672: csv.parse / csv.next / csv.flush do not leak (127.422)"
echo "-------------------------------------------------------------"

cat > leak.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
static long long in_use(void) { malloc_statistics_t st; malloc_zone_statistics(NULL, &st); return (long long)st.blocks_in_use; }
#define UNIT "blocks"
#define LIMIT (N / 2)        /* each leak was at least one block per call */
#elif defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 33))
#include <malloc.h>
static long long in_use(void) { struct mallinfo2 m = mallinfo2(); return (long long)m.uordblks; }
#define UNIT "bytes"
#define LIMIT (N * 8)        /* each leak was at least one 16-byte chunk per call */
#else
static long long in_use(void) { return -1; }
#define UNIT "bytes"
#define LIMIT 0
#endif
#include "tk_strhdr.h"
#include "tk_array.h"
#include "bytes_rt.h"
#include "csv.h"
int64_t tk_csv_parse_w(int64_t data);
int64_t tk_csv_reader_w(int64_t data, int64_t sep);
int64_t tk_csv_next_w(int64_t reader);
int64_t tk_csv_close_w(int64_t reader);
int64_t tk_csv_writer_w(int64_t sep);
int64_t tk_csv_writerow_w(int64_t writer, int64_t row);
int64_t tk_csv_flush_w(int64_t writer);
#define A(v) ((int64_t *)(intptr_t)(v))
#define N 20000
static void free_arr(int64_t h) { if (h) free(A(h) - TK_ARR_HDR); }
static void free_strs(int64_t h)
{
    if (!h) return;
    for (int64_t i = 0; i < A(h)[-1]; i++) {
        const char *s = (const char *)(intptr_t)A(h)[i];
        if (s && *s && tks_hdrlen(s) < 0) free((void *)s);   /* unsealed: toke's to drop */
    }
    free_arr(h);
}
static void free_row(int64_t cr) { if (cr) { free_strs(A(cr)[0]); free(A(cr)); } }
static int report(const char *what, long long grew)
{
    int ok = grew < LIMIT;
    printf("  %s: %s grew %lld %s in use over %d calls (limit %d)\n",
           ok ? "PASS" : "FAIL", what, grew, UNIT, N, (int)LIMIT);
    return ok;
}
static const char doc[] = "id,name,city\n1,ann,oslo\n2,bob,rome\n3,cy,lima\n";
static int64_t bytes(void) { return tk_bytes_pack((const uint8_t *)doc, sizeof doc - 1); }

int main(void)
{
    if (in_use() < 0) { printf("CONFORM-SKIP: no allocator statistics on this platform\n"); return 77; }
    int bad = 0;
    for (int warm = 0; warm < 2; warm++) {          /* warm the allocator's caches */
        int64_t b = bytes(); int64_t p = tk_csv_parse_w(b);
        for (int64_t i = 0; p && i < A(p)[-1]; i++) free_row(A(p)[i]);
        free_arr(p); free_arr(b);
    }

    long long t0 = in_use();
    for (int k = 0; k < N; k++) {
        int64_t b = bytes();
        int64_t p = tk_csv_parse_w(b);
        for (int64_t i = 0; p && i < A(p)[-1]; i++) free_row(A(p)[i]);
        free_arr(p); free_arr(b);
    }
    bad += !report("csv.parse", in_use() - t0);

    t0 = in_use();
    for (int k = 0; k < N; k++) {
        int64_t b = bytes();
        int64_t r = tk_csv_reader_w(b, 0);
        int64_t cr;
        while (r && (cr = tk_csv_next_w(r)) != 0) free_row(cr);
        tk_csv_close_w(r); free_arr(b);
    }
    bad += !report("csv.reader + csv.next", in_use() - t0);

    t0 = in_use();
    for (int k = 0; k < N; k++) {
        int64_t w = tk_csv_writer_w(0);
        int64_t row = tk_arr_alloc(3, 3);
        A(row)[0] = (int64_t)(intptr_t)"a"; A(row)[1] = (int64_t)(intptr_t)"b,c";
        A(row)[2] = (int64_t)(intptr_t)"d";
        tk_csv_writerow_w(w, row);
        free_arr(tk_csv_flush_w(w));
        csv_writer_free((TkCsvWriter *)(intptr_t)w); free_arr(row);
    }
    bad += !report("csv.flush", in_use() - t0);
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
CF=(-std=c99 -D_GNU_SOURCE -g -O1 -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects csv str -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set: see stderr above"; exit 1; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o leak leak.c "${LS[@]}" -lz -lpthread -lm > leak.cc 2>&1; then
    echo "  FAIL: C driver did not build"; sed 's/^/        /' leak.cc | head -6
    FAIL=$((FAIL + 1))
else
    ./leak > leak.out 2>&1; rc=$?
    if [ "${rc}" -eq 77 ]; then cat leak.out; exit 0; fi
    grep -E '^  (PASS|FAIL)' leak.out
    PASS=$((PASS + $(grep -c '^  PASS' leak.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' leak.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' leak.out; then
        echo "  FAIL: C driver exited ${rc}"; tail -6 leak.out | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi
echo "-------------------------------------------------------------"
echo "C1672: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
