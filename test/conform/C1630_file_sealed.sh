#!/usr/bin/env bash
# C1630_file_sealed.sh -- std.file hands toke sealed strings, and file.c and
# file_glue.c read with tk_strlen (story 127.295, S1 step 5 tranche 13;
# runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
# file.c's producers stay malloc'd (their C callers free them); the glue
# makes the sealed copy at the boundary and frees the original.
#
# THE FIXTURE is a directory tree made here: d/a.txt, d/b.txt, d/sub/c.txt,
# and d/lines.txt holding "one\r\ntwo\n\nthree" (CRLF, an empty line, no
# final newline).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), through the glue the compiler calls:
#        - tk_file_realpath_w: sealed, equal to realpath(3);
#        - tk_file_list_w / tk_file_listdir_w: every name sealed, the set
#          {a.txt, b.txt, lines.txt, sub};
#        - tk_file_listall_w: every relative path sealed, the set
#          {a.txt, b.txt, lines.txt, sub/c.txt};
#        - tk_file_readlines_w: one, two, "" and three, each sealed (the
#          empty line a sealed "");
#        - tk_file_listglob_w: d/a.txt and d/b.txt, sealed;
#        - tk_file_tempdir_w: a sealed copy of $TMPDIR that keeps its bytes
#          after setenv("TMPDIR") changes the environment under it;
#        - the readers, every argument sealed: mkdir (file_mkdir_p),
#          writelines, appendline, listall of a sealed dir, file.read of the
#          result, then file_rmdir_r of the sealed path, give exactly the
#          bytes and the outcome the same calls give on unsealed copies.
#   2. toke, under TK_STR_VERIFY=1: file.list, file.listall, file.realpath,
#      file.tempdir and their .len.
#
# Negative control: C1630_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base 55306a30's src/stdlib + stdlib: 2 passed, 6 failed -- the readers
#   case and the toke case pass there (an unsealed string read with strlen
#   was always correct); every producer case reports "not sealed".  There
#   tk_file_tempdir_w hands over getenv()'s own pointer, which Darwin's
#   setenv frees when TMPDIR is set again (ASan: heap-use-after-free on the
#   first draft of this harness), so that case checks the header before it
#   reads the bytes.
# Measured: this branch 8 passed (7 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1630_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1630_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1630: std.file results sealed; file.c and file_glue.c read with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

mkdir -p d/sub one
printf 'A' > d/a.txt
printf 'B' > d/b.txt
printf 'C' > d/sub/c.txt
printf 'one\r\ntwo\n\nthree' > d/lines.txt
printf 'x' > one/only.txt

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include "tk_strhdr.h"
#include "tk_array.h"
#include "file.h"
#include "capabilities.h"
#include "tk_runtime.h"   /* tk_current_error (thread-local) */
int64_t tk_file_realpath_w(int64_t path);
int64_t tk_file_list_w(int64_t dir);
int64_t tk_file_listdir_w(int64_t dir);
int64_t tk_file_listall_w(int64_t dir);
int64_t tk_file_readlines_w(int64_t path);
int64_t tk_file_listglob_w(int64_t pattern);
int64_t tk_file_tempdir_w(void);
int64_t tk_file_read_w(int64_t path);
int64_t tk_file_mkdir_w(int64_t path);
int64_t tk_file_writelines_w(int64_t path, int64_t lines);
int64_t tk_file_appendline_w(int64_t path, int64_t line);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.60s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
static int64_t alen(int64_t a) { return ((int64_t *)(intptr_t)a)[-1]; }
static int64_t aget(int64_t a, int64_t i) { return ((int64_t *)(intptr_t)a)[i]; }
static int cmp(const void *a, const void *b)
{ return strcmp(*(const char *const *)a, *(const char *const *)b); }
/* Every element sealed, and the sorted elements equal want[0..n). */
static int set_eq(int64_t a, const char **want, int n)
{
    if (!a) { printf("        (no array; err slot %lld)\n", (long long)tk_current_error); return 0; }
    if (alen(a) != n) { printf("        (%lld elements, want %d)\n", (long long)alen(a), n); return 0; }
    const char *got[16];
    int ok = 1;
    for (int i = 0; i < n; i++) { got[i] = P(aget(a, i)); ok &= sealed_eq(got[i], NULL); }
    qsort(got, (size_t)n, sizeof got[0], cmp);
    for (int i = 0; i < n; i++)
        if (strcmp(got[i], want[i]) != 0) { printf("        ([%d] '%s' want '%s')\n", i, got[i], want[i]); ok = 0; }
    return ok;
}
/* The reader calls, every argument a sealed string (sealed=1) or a plain
 * malloc'd copy (sealed=0).  Returns the file's bytes after the calls, or
 * NULL; *rm_ok is file_rmdir_r's outcome. */
static char *readers(int sealed, int *rm_ok)
{
    const char *dir_c = sealed ? "r_s/x/y" : "r_u/x/y";
    const char *top_c = sealed ? "r_s" : "r_u";
    const char *file_c = sealed ? "r_s/x/y/f.txt" : "r_u/x/y/f.txt";
    const char *l1 = "alpha", *l2 = "", *l3 = "gamma delta", *l4 = "tail";
#define S_(x) (sealed ? (const char *)tks_strdup(x) : (const char *)strdup(x))
    const char *dir = S_(dir_c), *top = S_(top_c), *file = S_(file_c);
    if (sealed && (tks_hdrlen(dir) < 0 || tks_hdrlen(file) < 0)) return NULL;
    if (!tk_file_mkdir_w(I(dir))) { printf("        (%s %s failed)\n", top_c, "mkdir"); return NULL; }
    int64_t arr = tk_arr_alloc(3, 3);
    ((int64_t *)(intptr_t)arr)[0] = I(S_(l1));
    ((int64_t *)(intptr_t)arr)[1] = I(S_(l2));
    ((int64_t *)(intptr_t)arr)[2] = I(S_(l3));
    if (!tk_file_writelines_w(I(file), arr)) { printf("        (%s %s failed)\n", top_c, "writelines"); return NULL; }
    if (!tk_file_appendline_w(I(file), I(S_(l4)))) { printf("        (%s %s failed)\n", top_c, "appendline"); return NULL; }
    int64_t all = tk_file_listall_w(I(top));
    if (!all || alen(all) != 1 || strcmp(P(aget(all, 0)), "x/y/f.txt") != 0) {
        printf("        (listall of %s)\n", top_c); return NULL;
    }
    int64_t body = tk_file_read_w(I(file));
    if (!body || tk_current_error) { printf("        (%s %s failed)\n", top_c, "read"); return NULL; }
    BoolFileResult rr = file_rmdir_r(top);
    *rm_ok = !rr.is_err && !file_exists(top);
#undef S_
    return strdup(P(body));
}
int main(void)
{
    char a0[] = "seal", a1[] = "--allow-all";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);              /* the wrappers check fs.read/fs.write */
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    char rp[PATH_MAX];
    if (!realpath("d", rp)) return 2;
    int64_t r = tk_file_realpath_w(I("d"));
    CHECK(sealed_eq(P(r), rp), "tk_file_realpath_w: sealed, equal to realpath(3)");

    const char *ls[] = { "a.txt", "b.txt", "lines.txt", "sub" };
    int k = set_eq(tk_file_list_w(I("d")), ls, 4);
    k &= set_eq(tk_file_listdir_w(I("d")), ls, 4);
    CHECK(k, "tk_file_list_w / tk_file_listdir_w: every name sealed");

    const char *la[] = { "a.txt", "b.txt", "lines.txt", "sub/c.txt" };
    CHECK(set_eq(tk_file_listall_w(I("d")), la, 4),
          "tk_file_listall_w: every relative path sealed");

    int64_t rl = tk_file_readlines_w(I("d/lines.txt"));
    const char *want_l[] = { "one", "two", "", "three" };
    k = rl && alen(rl) == 4;
    if (!k) printf("        (readlines: %lld)\n", rl ? (long long)alen(rl) : -1LL);
    for (int i = 0; k && i < 4; i++) k &= sealed_eq(P(aget(rl, i)), want_l[i]);
    CHECK(k, "tk_file_readlines_w: one, two, \"\", three -- each sealed, in order");

    const char *lg[] = { "d/a.txt", "d/b.txt" };
    CHECK(set_eq(tk_file_listglob_w(I("d/[ab].txt")), lg, 2),
          "tk_file_listglob_w: every match sealed");

    setenv("TMPDIR", "/c1630/first", 1);
    int64_t t = tk_file_tempdir_w();
    setenv("TMPDIR", "/c1630/a-much-longer-second-value", 1);
    /* Not sealed means getenv's own pointer, which this setenv may have
     * freed (Darwin does): say so without reading it. */
    if (t && tks_hdrlen(P(t)) < 0) printf("        (not sealed: the environment's pointer)\n");
    CHECK(t && tks_hdrlen(P(t)) >= 0 && sealed_eq(P(t), "/c1630/first"),
          "tk_file_tempdir_w: a sealed copy, unchanged when TMPDIR is set again");

    int rm_s = 0, rm_u = 0;
    char *bs = readers(1, &rm_s), *bu = readers(0, &rm_u);
    const char *want_b = "alpha\n\ngamma delta\ntail\n";
    k = bs && bu && strcmp(bs, want_b) == 0 && strcmp(bs, bu) == 0 && rm_s && rm_u;
    if (!k) printf("        (sealed '%s' rm %d, unsealed '%s' rm %d)\n",
                   bs ? bs : "(null)", rm_s, bu ? bu : "(null)", rm_u);
    CHECK(k, "readers with sealed arguments (mkdir, writelines, appendline, listall, read, rmdir_r) == unsealed");
    free(bs); free(bu);
    return bad;
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
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
        tail -5 seal.out | sed 's/^/        | /'
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=file:std.file;
f=show(tag:str;xs:@(str)):i64{
  io.println("\(tag)=\(xs.get(0)) \(xs.get(0).len) \(xs.len)");
  <0
};
f=main():i64{
  mt file.list("one") {$ok:xs show("list";xs); $err:e io.println("list failed")};
  mt file.listall("one") {$ok:xs show("all";xs); $err:e io.println("listall failed")};
  mt file.realpath("one") {$ok:p io.println("rp=\(p) \(p.len)"); $err:e io.println("realpath failed")};
  let t=file.tempdir();
  io.println("t=\(t) \(t.len)");
  <0
};
TOKE
RP="$(cd one && pwd -P)"
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TMPDIR=/c1630/tmp TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    if [ "${rc}" -eq 0 ] && grep -a -q -x -F -- "list=only.txt 8 1" p.out &&
       grep -a -q -x -F -- "all=only.txt 8 1" p.out &&
       grep -a -q -x -F -- "rp=${RP} ${#RP}" p.out &&
       grep -a -q -x -F -- "t=/c1630/tmp 10" p.out; then
        pass "toke: file.list, listall, realpath, tempdir and .len under TK_STR_VERIFY=1"
    else
        fail "toke (rc=${rc}): '$(head -5 p.out | tr '\n' '|')'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1630: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
