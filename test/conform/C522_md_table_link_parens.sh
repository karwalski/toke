#!/usr/bin/env bash
# C522_md_table_link_parens.sh — a Markdown table-cell link destination
# with parentheses renders as cmark renders it outside a table (story
# 121.49 item 2).
#
# THE DEFECT, measured on main (7014e929):
#   md_inline_cell ended a table-cell link destination at the first ')', so
#   | [Foo](https://en.wikipedia.org/wiki/Foo_(bar)) |
#   rendered <a href="https://en.wikipedia.org/wiki/Foo_(bar">Foo</a>) --
#   a truncated href and a stray ")" after the link.  The same for
#   [x](javascript:alert(1)): href="" (C441 still holds) and a stray ")".
#   `a\)b` kept its backslash and ended at the escaped paren; <a)b> was not
#   recognised; an unbalanced `[x](a(b) tail` became a link.
#
# NOW: balanced unescaped parentheses, backslash-escaped punctuation, and
# the <...> form are scanned as CommonMark scans them.  The oracle is cmark
# itself: each case is rendered once in a table cell and once as a
# paragraph, and the cell's inner HTML must equal the paragraph's.  Cases
# where the two legitimately differ (cmark percent-encodes a backslash in an
# href; titles and spaces in a bare destination are not handled in cells)
# are left out and named in md.c.  The scheme filter sees the unescaped
# destination: `javascript\:` gets href="".  Built with ASan+UBSan.
#
# Negative control: C522_MD_C=<file> builds against another md.c; main's
# fails 9 of the 12 parity cases.
#
# Story: 121.49

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
MD_C="${C522_MD_C:-${S}/md.c}"
CM="${REPO_ROOT}/stdlib/vendor/cmark/src"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c522_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C522: md table-cell link destinations with parentheses match cmark"
echo "-------------------------------------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "md.h"

static int fails = 0, checks = 0;

/* Inner HTML of the first <open>...</close> in html, cut in place. */
static char *inner(char *html, const char *open, const char *close) {
    char *p = html ? strstr(html, open) : NULL;
    if (!p) return NULL;
    p += strlen(open);
    char *e = strstr(p, close);
    if (e) *e = '\0';
    return p;
}

int main(void) {
    const char *cases[] = {
        "[Foo](https://en.wikipedia.org/wiki/Foo_(bar))",
        "[x](javascript:alert(1))",
        "[x](a(b)c(d)) tail",
        "[x](a((b))) tail",
        "[x](a(b) tail",
        "[x](a\\)b)",
        "[x](<a)b>)",
        "[x](<javascript:x>)",
        "[x](javascript\\:alert(1))",
        "[f(x)](u)",
        "[x]()",
        "[x](https://e.org/q?a=1&b=2)",
    };
    char src[1024];
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        snprintf(src, sizeof src, "| h |\n|---|\n| %s |\n", cases[i]);
        char *t = (char *)md_render(src);
        snprintf(src, sizeof src, "%s\n", cases[i]);
        char *c = (char *)md_render(src);
        char *ti = inner(t, "<td>", "</td>");
        char *ci = inner(c, "<p>", "</p>");
        checks++;
        if (ti && ci && strcmp(ti, ci) == 0) {
            printf("  PASS: %-50s -> %s\n", cases[i], ti);
        } else {
            printf("  FAIL: %s\n        table %s\n        cmark %s\n", cases[i],
                   ti ? ti : "<none>", ci ? ci : "<none>");
            fails++;
        }
        free(t); free(c);
    }
    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# shellcheck disable=SC2046
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -Wno-pedantic -I"${CM}" -iquote "${S}" \
        -o "${WORK}/t" "${WORK}/t.c" "${MD_C}" \
        $(ls "${CM}"/*.c | grep -v '/main\.c$') >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t"
rc=$?
echo ""
[ "${rc}" -eq 0 ] && echo "C522: PASS" || echo "C522: FAIL (exit ${rc})"
exit "${rc}"
