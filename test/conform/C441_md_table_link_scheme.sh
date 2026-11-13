#!/usr/bin/env bash
# C441_md_table_link_scheme.sh — a Markdown table-cell link cannot carry a
# script URL, and table-cell inline nesting has a constant depth (story
# 121.16: PAR-05 residue, PAR-08 re-scope).
#
# THE DEFECT (measured on main before 121.16).
#   124.3 moved cmark to SAFE mode, which renders a `javascript:` link as
#   href="".  But pipe tables are rendered by md.c itself and re-injected
#   into cmark's output as trusted HTML, so cmark's URL filter never sees a
#   table-cell link: `| [x](javascript:alert(1)) |` rendered as
#   <a href="javascript:alert(1)">x</a> -- stored XSS in any page that
#   renders user Markdown containing a table.
#
# WHAT THIS DRIVES (ASan+UBSan build of md.c + vendored cmark):
#   * each dangerous URL in a table cell -> the output has no href whose
#     value, read the way a browser reads it, starts with that scheme:
#     javascript:, JaVaScRiPt:, a leading space, a tab inside the scheme,
#     vbscript:, file:, data:text/html;
#   * parity with cmark for every URL cmark itself parses as a link: the
#     table cell's href is empty exactly when cmark's (outside a table) is;
#   * controls: https (with & escaped), a relative path, mailto:, and
#     data:image/png keep their href;
#   * PAR-08 (re-scoped, green on main too): a 400 KB cell of nested
#     `**[*` openers renders under a 256 KB stack.  md_inline_cell recurses
#     only into a span that excludes its own closing delimiter (bold: no
#     "**"; italic: no "*"; link text: no "]"), so the depth is at most 3
#     whatever the input -- the audit's "len/4 deep" does not occur.
#
# Negative control: C441_MD_C=<file> builds against another md.c; main's
# fails the seven dangerous-URL checks and the parity check.
#
# Story: 121.16

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
MD_C="${C441_MD_C:-${S}/md.c}"
CM="${REPO_ROOT}/stdlib/vendor/cmark/src"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c441_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C441: md table-cell links refuse script URLs; cell nesting depth is constant"
echo "---------------------------------------------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/resource.h>
#include "md.h"

static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

/* The first href value in html, decoded the way a browser reads the
 * attribute: &amp; &quot; &lt; &gt; resolved, leading C0/space dropped,
 * tab/LF/CR removed, lower-cased.  Empty when there is no href. */
static void first_href(const char *html, char *out, size_t cap) {
    out[0] = '\0';
    const char *h = strstr(html, "href=\"");
    if (!h) return;
    h += 6;
    size_t k = 0; int lead = 1;
    while (*h && *h != '"' && k + 1 < cap) {
        char c = *h;
        if (!strncmp(h, "&amp;", 5)) { c = '&'; h += 4; }
        else if (!strncmp(h, "&quot;", 6)) { c = '"'; h += 5; }
        else if (!strncmp(h, "&lt;", 4)) { c = '<'; h += 3; }
        else if (!strncmp(h, "&gt;", 4)) { c = '>'; h += 3; }
        h++;
        if (lead && (unsigned char)c <= 0x20) continue;
        lead = 0;
        if (c == '\t' || c == '\n' || c == '\r') continue;
        out[k++] = (char)tolower((unsigned char)c);
    }
    out[k] = '\0';
}

/* 127.295: md_render's result is a sealed string -- never freed. */
static const char *table_with(const char *url) {
    size_t n = strlen(url) + 64;
    char *src = malloc(n);
    snprintf(src, n, "| h |\n|---|\n| [x](%s) |\n", url);
    const char *html = md_render(src);
    free(src);
    return html;
}

int main(void) {
    struct { const char *url, *scheme, *label; } bad[] = {
        { "javascript:alert(1)",          "javascript:", "javascript:" },
        { "JaVaScRiPt:alert(1)",          "javascript:", "mixed-case JaVaScRiPt:" },
        { " javascript:alert(1)",         "javascript:", "a leading space" },
        { "java\tscript:alert(1)",        "javascript:", "a tab inside the scheme" },
        { "vbscript:msgbox(1)",           "vbscript:",   "vbscript:" },
        { "file:///etc/passwd",           "file:",       "file:" },
        { "data:text/html,<script>alert(1)</script>", "data:", "data:text/html" },
    };
    char href[512], msg[200];
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        const char *html = table_with(bad[i].url);
        first_href(html, href, sizeof href);
        snprintf(msg, sizeof msg, "table-cell link with %s has no script href (got \"%s\")",
                 bad[i].label, href);
        CHECK(strncmp(href, bad[i].scheme, strlen(bad[i].scheme)) != 0, msg);
    }

    /* parity with cmark: URLs cmark parses as a link destination */
    const char *par[] = { "javascript:alert(1)", "JaVaScRiPt:x", "vbscript:x", "file:///x",
                          "data:text/html,x", "data:image/png;base64,AAAA",
                          "https://example.com/a?b=1&c=2", "/rel/path", "mailto:a@b.c", "#frag" };
    int mism = 0;
    for (size_t i = 0; i < sizeof par / sizeof par[0]; i++) {
        const char *t = table_with(par[i]);
        size_t n = strlen(par[i]) + 16;
        char *src = malloc(n); snprintf(src, n, "[x](%s)\n", par[i]);
        const char *c = md_render(src); free(src);
        char ht[512], hc[512];
        first_href(t, ht, sizeof ht); first_href(c, hc, sizeof hc);
        if ((ht[0] == '\0') != (hc[0] == '\0')) {
            printf("        parity: %-32s table \"%s\"  cmark \"%s\"\n", par[i], ht, hc);
            mism++;
        }
    }
    CHECK(mism == 0, "table-cell href is empty exactly when cmark's own is, for 10 URLs");

    struct { const char *url, *want, *label; } good[] = {
        { "https://example.com/a?b=1&c=2", "https://example.com/a?b=1&c=2", "https with a query" },
        { "/rel/path",                     "/rel/path",                     "a relative path" },
        { "mailto:a@b.c",                  "mailto:a@b.c",                  "mailto:" },
        { "data:image/png;base64,AAAA",    "data:image/png;base64,aaaa",    "data:image/png" },
    };
    for (size_t i = 0; i < sizeof good / sizeof good[0]; i++) {
        const char *html = table_with(good[i].url);
        first_href(html, href, sizeof href);
        snprintf(msg, sizeof msg, "control: %s keeps its href", good[i].label);
        CHECK(strcmp(href, good[i].want) == 0, msg);
    }

    /* PAR-08: 400 KB of nested openers in one cell, on a 256 KB stack */
    size_t N = 400000;
    char *cell = malloc(N + 32);
    size_t k = 0;
    memcpy(cell, "| h |\n|---|\n| ", 14); k = 14;
    const char pat[] = "**[*";
    while (k < N) { cell[k] = pat[k % 4]; k++; }
    memcpy(cell + k, " |\n", 4);
    const char *html = md_render(cell);
    CHECK(html && strstr(html, "<table>") != NULL,
          "PAR-08: a 400 KB cell of nested **[* openers renders (constant nesting depth)");
    free(cell);

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# 127.295: md.c's results are sealed strings (str.c's string heap), so the
# driver takes std.md's link set -- runtime, str.c, the vendored cmark
# sources -- from stdlib_link_set.sh as cached objects, and compiles the md.c
# under test itself (--except md.c) so the negative control still swaps it.
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -Wno-pedantic -I"${CM}" -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --except md.c --cc "${CC}" \
        --objects md -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (md): see stderr above"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o "${WORK}/t" "${WORK}/t.c" "${MD_C}" \
        "${LINK[@]}" -lm -lpthread >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
# 256 KB stack for the whole driver: a len/4-deep recursion over the
# 400 KB cell would need ~100k frames.
( ulimit -s 256 2>/dev/null; ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" )
rc=$?
echo ""
[ "${rc}" -eq 0 ] && echo "C441: PASS" || echo "C441: FAIL (exit ${rc})"
exit "${rc}"
