#!/usr/bin/env bash
# C440_toon_arr_count_oom.sh — toon.arr never trusts the declared [count],
# and every toon.c allocation failure fails cleanly (story 121.16: PAR-06,
# PAR-10).
#
# THE DEFECTS (measured on main before 121.16).
#   PAR-06  toon_arr used the header's `name[count]` as the array length when
#           it was non-zero: `d[100]{id}:\n1\n` returned len 100 with elements
#           1..99 never written (uninitialised heap pointers handed to the
#           program); `d[-5]{id}:...` malloc'd (size_t)-80, got NULL, skipped
#           the fill and returned len 2^64-5 over a NULL array; and a blank
#           line took a row slot as an empty value, so `d{id}:\n1\n\n2\n`
#           returned ["1", ""] and lost "2".
#   PAR-10  parse_schema, toon_arr and toon_from_json wrote through unchecked
#           malloc results -- a NULL deref on allocation failure.
#
# WHAT THIS DRIVES (ASan+UBSan build that includes toon.c with malloc,
# calloc and realloc routed through a counter that fails the Nth call):
#   the three count cases and the blank-line case (lengths and values; an
#   element is only dereferenced when the returned length is plausible, so a
#   defective build FAILs rather than crashing); a declared count below the
#   rows present still keeps the first N; then an OOM sweep that fails
#   allocation 0..K of toon_dec/toon_str/toon_arr/toon_to_json/toon_from_json
#   (each in a forked child) and requires every child to exit cleanly.
#
# Negative control: C440_TOON_C=<file> builds against another toon.c; main's
# fails the four length/value checks and crashes in the OOM sweep.
#
# Story: 121.16

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
TOON_C="${C440_TOON_C:-${S}/toon.c}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c440_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C440: toon.arr bounds its length by the rows present; toon.c survives OOM"
echo "-------------------------------------------------------------------------"

cp "${TOON_C}" "${WORK}/toon_under_test.c"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static long fa_n = -1;             /* fail the allocation with this index */
static long fa_i = 0;
static void *fa_malloc(size_t n)             { return fa_i++ == fa_n ? NULL : malloc(n); }
static void *fa_calloc(size_t a, size_t b)   { return fa_i++ == fa_n ? NULL : calloc(a, b); }
static void *fa_realloc(void *p, size_t n)   { return fa_i++ == fa_n ? NULL : realloc(p, n); }
#define malloc(n)     fa_malloc(n)
#define calloc(a, b)  fa_calloc(a, b)
#define realloc(p, n) fa_realloc(p, n)
#include "toon_under_test.c"
#undef malloc
#undef calloc
#undef realloc

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

static ToonArrayResult arr_of(const char *src, const char *key) {
    Toon t; t.raw = src;
    return toon_arr(t, key);
}

static void exercise(void) {
    const char *docs[] = {
        "d[3]{id,name}:\n1|a\n2|b\n3|c\n",
        "d[2]{id}:\n1\n2\n",
        "{id,name}:\n7|x\n",
    };
    for (int i = 0; i < 3; i++) {
        ToonResult d = toon_dec(docs[i]);
        if (d.is_err) continue;
        StrToonResult s = toon_str(d.ok, "id");
        if (!s.is_err) free((char *)s.ok);
        ToonArrayResult a = toon_arr(d.ok, "id");
        if (!a.is_err) {
            for (uint64_t k = 0; k < a.ok.len; k++) free((char *)a.ok.data[k].raw);
            free(a.ok.data);
        }
        const char *j = toon_to_json(docs[i]);
        if (j && strcmp(j, "[]") != 0) free((char *)j);   /* "[]" may be a literal */
    }
    const char *t1 = toon_from_json("{\"id\":1,\"name\":\"x\"}");
    if (t1 && *t1) free((char *)t1);
    const char *t2 = toon_from_json("[{\"id\":1,\"n\":\"a\"},{\"id\":2,\"n\":\"b\"}]");
    if (t2 && *t2) free((char *)t2);
}

int main(void) {
    ToonArrayResult a;

    a = arr_of("d[100]{id}:\n1\n", "id");
    CHECK(!a.is_err && a.ok.len == 1 && strcmp(a.ok.data[0].raw, "1") == 0,
          "d[100]{id} over one row -> length 1 (was 100, elements 1..99 uninitialised)");

    a = arr_of("d[-5]{id}:\n1\n2\n", "id");
    CHECK(!a.is_err && a.ok.len == 2 && a.ok.data &&
          strcmp(a.ok.data[0].raw, "1") == 0 && strcmp(a.ok.data[1].raw, "2") == 0,
          "d[-5]{id} -> the two rows present (was length 2^64-5 over NULL)");

    a = arr_of("d[2000000000]{id,v}:\n1|x\n2|y\n", "v");
    CHECK(!a.is_err && a.ok.len == 2 && strcmp(a.ok.data[1].raw, "y") == 0,
          "d[2000000000]{id,v} -> length 2, values from the rows present");

    a = arr_of("d{id}:\n1\n\n2\n", "id");
    CHECK(!a.is_err && a.ok.len == 2 && a.ok.len <= 2 &&
          strcmp(a.ok.data[0].raw, "1") == 0 && strcmp(a.ok.data[1].raw, "2") == 0,
          "a blank line is skipped, not read as an empty row (was [\"1\",\"\"], \"2\" lost)");

    a = arr_of("d[2]{id}:\n5\n6\n7\n", "id");
    CHECK(!a.is_err && a.ok.len == 2 && strcmp(a.ok.data[0].raw, "5") == 0 &&
          strcmp(a.ok.data[1].raw, "6") == 0,
          "control: a declared count below the rows present keeps the first N");

    a = arr_of("d[3]{id,name}:\n1|a\n2|b\n3|c\n", "name");
    CHECK(!a.is_err && a.ok.len == 3 && strcmp(a.ok.data[2].raw, "c") == 0,
          "control: an honest header decodes unchanged");

    /* how many allocations does one exercise() make? */
    fa_n = -1; fa_i = 0;
    exercise();
    long total = fa_i;
    int crashed = 0, first = -1;
    for (long k = 0; k < total; k++) {
        pid_t pid = fork();
        if (pid == 0) { fa_n = k; fa_i = 0; exercise(); _exit(0); }
        int st = 0;
        waitpid(pid, &st, 0);
        if (!WIFEXITED(st) || WEXITSTATUS(st) != 0) { crashed++; if (first < 0) first = (int)k; }
    }
    char msg[160];
    snprintf(msg, sizeof msg, "OOM sweep: allocation 0..%ld failed in turn, %d crash(es)%s",
             total - 1, crashed, "");
    CHECK(total > 10 && crashed == 0, msg);
    if (crashed) printf("        first crashing allocation index: %d\n", first);

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -iquote "${WORK}" \
        -o "${WORK}/t" "${WORK}/t.c" >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:allocator_may_return_null=1" "${WORK}/t" 2>"${WORK}/stderr.log"
rc=$?
if [ "${rc}" -ne 0 ] && [ "${rc}" -ne 1 ]; then
    echo "  FAIL: driver died (exit ${rc}):"; head -8 "${WORK}/stderr.log" | sed 's/^/      /'
fi
echo ""
[ "${rc}" -eq 0 ] && echo "C440: PASS" || echo "C440: FAIL (exit ${rc})"
exit "${rc}"
