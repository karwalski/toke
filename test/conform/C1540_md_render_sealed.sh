#!/usr/bin/env bash
# C1540_md_render_sealed.sh -- std.md hands toke sealed strings, and md.c
# reads its inputs with tk_strlen (story 127.295, S1 step 5 tranche 10;
# runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), every result sealed with the exact bytes:
#        - md_render of plain Markdown, of a sealed input (tks_strdup), of a
#          pipe table (the re-inject path), of a source holding every
#          placeholder prefix (the no-tables-lifted path), of "" and of NULL;
#        - tk_md_render_w (the glue);
#        - md_render_file's .ok, and its $err path (no result);
#        - fault injection: every malloc/realloc/calloc md.c itself makes
#          (counted on a clean run, bounded at 400) fails in turn and
#          md_render still returns a sealed C string -- the copy-of-source
#          fallback is sealed too.
#   2. toke, under TK_STR_VERIFY=1: md.render and md.renderfile give the
#      expected HTML and `.len`, and a result passes through str calls.
#
# Negative control: C1540_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files and vendored cmark).
#   Base d530faaa's src/stdlib + stdlib: 2 passed, 6 failed -- every
#   producer case reports "not sealed" (md_render handed out cmark's
#   malloc'd buffer); the toke cases pass there, because an unsealed string
#   read with strlen was always correct.
# Measured: this branch 8 passed (6 C + 2 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1540_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
CM="${S}/../../stdlib/vendor/cmark/src"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1540_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1540: std.md results sealed; md.c reads with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

printf '# Doc\n\n| a | b |\n|---|---|\n| 1 | [x](https://e.org) |\n' > doc.md

# ── 1. C ─────────────────────────────────────────────────────────────────────
# md.c is compiled here with every allocator call routed through a counter
# that can fail the Nth one; the rest of std.md's link set (runtime, str.c,
# vendored cmark) is untouched.
cat > fault.h <<'C'
#include <stdlib.h>
void *c1540_malloc(size_t n);
void *c1540_realloc(void *p, size_t n);
void *c1540_calloc(size_t c, size_t n);
#define malloc(n) c1540_malloc(n)
#define realloc(p, n) c1540_realloc(p, n)
#define calloc(c, n) c1540_calloc(c, n)
C
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "md.h"
int64_t tk_md_render_w(int64_t src);
static int fail_at = 0, calls = 0;
void *c1540_malloc(size_t n) { if (++calls == fail_at) return NULL; return malloc(n); }
void *c1540_realloc(void *p, size_t n) { if (++calls == fail_at) return NULL; return realloc(p, n); }
void *c1540_calloc(size_t c, size_t n) { if (++calls == fail_at) return NULL; return calloc(c, n); }
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int sealed(const char *s, int quiet)
{
    if (!s) { if (!quiet) printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { if (!quiet) printf("        (not sealed: '%.60s')\n", s); return 0; }
    if ((size_t)h != strlen(s)) {
        if (!quiet) printf("        (header %lld, strlen %zu)\n", (long long)h, strlen(s));
        return 0;
    }
    return 1;
}
static int sealed_eq(const char *s, const char *want)
{
    if (!sealed(s, 0)) return 0;
    if (want && strcmp(s, want) != 0) { printf("        (got '%s')\n", s); return 0; }
    return 1;
}
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static const char *TABLE = "intro\n\n| h | k |\n|---|---|\n| *a* | `b` |\n| [x](javascript:1) | y |\n\nend\n";
static const char *TABLE_HTML =
    "<p>intro</p>\n<table>\n<thead>\n<tr><th>h</th><th>k</th></tr>\n</thead>\n<tbody>\n"
    "<tr><td><em>a</em></td><td><code>b</code></td></tr>\n"
    "<tr><td><a href=\"\">x</a></td><td>y</td></tr>\n</tbody>\n</table>\n\n<p>end</p>\n";
int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];

    CHECK(sealed_eq(md_render("# Hi *there*\n"), "<h1>Hi <em>there</em></h1>\n") &&
          sealed_eq(md_render(tks_strdup("para & <b>\n")), "<p>para &amp; <!-- raw HTML omitted --></p>\n"),
          "md_render: plain and sealed inputs give sealed results");
    CHECK(sealed_eq(md_render(TABLE), TABLE_HTML),
          "md_render: a pipe table (re-injected) gives a sealed result");
    const char *ph = md_render("| h |\n|---|\n| v |\n\nTKMDTABLEPLACEHOLDERQQQQQQQ\n");
    CHECK(sealed(ph, 0) && !strstr(ph, "<table>") && strstr(ph, "TKMDTABLEPLACEHOLDERQQQQQQQ"),
          "md_render: every placeholder prefix taken -- rendered without tables, sealed");
    const char *e = md_render(""), *n = md_render(NULL);
    CHECK(sealed_eq(e, "") && sealed_eq(n, "") &&
          sealed_eq(P(tk_md_render_w(I("*z*"))), "<p><em>z</em></p>\n"),
          "md_render(\"\"), md_render(NULL) and tk_md_render_w: sealed");
    snprintf(path, sizeof path, "%s/doc.md", dir);
    MdFileResult f = md_render_file(path);
    snprintf(path, sizeof path, "%s/no_such.md", dir);
    MdFileResult m = md_render_file(path);
    CHECK(!f.is_err && sealed(f.ok, 0) && strstr(f.ok, "<h1>Doc</h1>") &&
          strstr(f.ok, "<a href=\"https://e.org\">x</a>") &&
          m.is_err && m.ok == NULL && strcmp(m.err_msg, "file not found") == 0,
          "md_render_file: .ok sealed; a missing file is $err with no result");

    /* Fault injection, bounded: fail each of md.c's own allocations in turn. */
    fail_at = 0; calls = 0;
    const char *clean = md_render(TABLE);
    int total = calls, all = 1, same = 0;
    if (total > 400) total = 400;
    for (int k = 1; k <= total; k++) {
        fail_at = k; calls = 0;
        const char *r = md_render(TABLE);          /* must return, not crash */
        if (!sealed(r, 1)) { all = 0; printf("        (allocation %d: not sealed)\n", k); }
        else if (strcmp(r, TABLE_HTML) == 0) same++;
    }
    fail_at = 0;
    printf("        (%d allocations in md.c failed in turn; %d runs still gave the full table)\n", total, same);
    CHECK(sealed_eq(clean, TABLE_HTML) && total > 0 && all,
          "md_render: with any one of md.c's allocations failing, the result is still sealed");
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12); md.c itself is
# compiled with the fault allocator (--except md.c).
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -Wno-pedantic -I"${CM}" -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --except md.c --objects md -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -include "${WORK}/fault.h" -c -o md.o "${S}/md.c" > seal.cc 2>&1 \
     || ! "${CC}" "${CF[@]}" -o seal seal.c md.o "${LS[@]}" -lpthread -lm >> seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${WORK}" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=md:std.md;
i=s:std.str;
f=main():i64{
  let h=md.render("## T\n\n| a |\n|---|\n| 1 |\n");
  io.println("[\(s.trim(h))] \(h.len)");
  let r=mt md.renderfile("doc.md") {$ok:v v;$err:e e.msg};
  io.println("file=\(r.len) \(s.contains(r;"<td>1</td>"))");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p --allow-read > p.out 2>&1; rc=$?
    want1="[<h2>T</h2>
<table>
<thead>
<tr><th>a</th></tr>
</thead>
<tbody>
<tr><td>1</td></tr>
</tbody>
</table>] 103"
    got1="$(grep -a -v '^file=' p.out)"
    if [ "${rc}" -eq 0 ] && [ "${got1}" = "${want1}" ]; then
        pass "toke: md.render gives the table HTML and .len under TK_STR_VERIFY=1"
    else
        fail "toke: md.render (rc=${rc}): $(printf '%s' "${got1}" | tr '\n' '|' | head -c 300)"
    fi
    if grep -a -q -x -F -- "file=153 true" p.out; then
        pass "toke: md.renderfile result reads through .len and str.contains"
    else
        fail "toke: md.renderfile: '$(grep -a '^file=' p.out | head -1)'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1540: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
