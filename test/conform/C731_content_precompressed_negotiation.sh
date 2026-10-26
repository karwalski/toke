#!/usr/bin/env bash
# C731_content_precompressed_negotiation.sh -- content_find_precompressed
# offers a .br/.gz/.zst variant only when Accept-Encoding admits its coding,
# q-values and "*" included, in the server's order (story 121.96).
#
# THE DEFECT, measured on main (837e5f81) with this script.
# src/stdlib/content.c's content_find_precompressed picked a precompressed
# variant of a file with strstr on the Accept-Encoding value:
#   - "gzip;q=0" (the client refusing gzip) got the .gz file -- the 121.94
#     defect again, for files on disk;
#   - "br" matched inside any coding name holding those letters
#     ("abrupt"), and a client's "*" got nothing;
#   - "GZIP" (names compare without case, RFC 9110 sec. 12.5.3) got nothing.
#
# THE FIX.  tk_gzip_accepted (tk_gzip_type.h, 121.94) is generalised to
# tk_coding_accepted(ae, coding) -- the same RFC 9110 sec. 12.5.3 reading
# for any content coding, tk_gzip_accepted now its "gzip" wrapper for the
# two gzip writers -- and content_find_precompressed asks it for br, gzip
# and zstd, in that (the server's) order, taking the first acceptable
# variant that exists.
#
# WHY NOT OVER THE WIRE.  The story asked for a wire test, but no server path
# calls content_find_precompressed: it has no prototype in a header, no
# caller in src/, no .tki entry, and none in toke-ooke or the website
# (checked 2026-10-08).  So this harness calls it directly (C, under
# ASan+UBSan) against real files in a temp directory; whatever later wires
# it into http.servedir inherits the rule.  The gzip writers' use of the
# same parser is asserted over the wire by C726 (6).
#
# Negative control: C731_STDLIB=<dir> builds against another stdlib tree.
# Against a git archive of main's src/stdlib (837e5f81) the harness cannot
# build its tk_coding_accepted section (the function does not exist), so it
# is compiled with -DC731_NO_CODING and only the file-picking cases run:
# 7 of 17 fail there (gzip;q=0 -> .gz, br;q=0,gzip -> .br, * -> none,
# br;q=0,gzip;q=0,* -> .br, abrupt -> .br, GZIP -> none, and
# br;q=1,gzip;q=0 -> .gz).  On this branch: 29 of 29 pass.
#
# Story: 121.96
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C731_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c731_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C731: precompressed variants follow Accept-Encoding (121.96)"
echo "----------------------------------------------------------------------"

cat > "${WORK}/h.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "tk_gzip_type.h"
char *content_find_precompressed(const char *path, const char *accept_encoding);

static int bad;
static void pick(const char *dir, const char *ae, const char *want)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/f.html", dir);
    char *got = content_find_precompressed(path, ae);
    const char *g = got ? got + strlen(path) : NULL;   /* the suffix, or NULL */
    if (g && !*g) g = "(the file itself)";
    int ok = want ? (g && strcmp(g, want) == 0) : g == NULL;
    printf("%s pick [%s] in %s -> %s (want %s)\n", ok ? "OK" : "BAD",
           ae ? ae : "(no header)", strrchr(dir, '/') + 1,
           g ? g : "none", want ? want : "none");
    bad += !ok;
    free(got);
}
#ifndef C731_NO_CODING
static void acc(const char *ae, const char *coding, int want)
{
    int got = tk_coding_accepted(ae, coding);
    printf("%s accepted [%s] %s -> %d (want %d)\n", got == want ? "OK" : "BAD",
           ae ? ae : "(no header)", coding, got, want);
    bad += got != want;
}
#endif

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *all = argv[1];   /* f.html + .br + .gz + .zst */
    const char *gz  = argv[2];   /* f.html + .gz + .zst        */
    pick(all, "gzip;q=0", NULL);              /* main: .gz */
    pick(all, "gzip;q=0, br", ".br");
    pick(all, "br;q=0, gzip", ".gz");
    pick(all, "gzip, br", ".br");             /* the server's order */
    pick(all, "zstd, gzip", ".gz");           /* .gz before .zst     */
    pick(all, "zstd", ".zst");
    pick(all, "*", ".br");                    /* main: none */
    pick(all, "*;q=0", NULL);
    pick(all, "br;q=0, gzip;q=0, *", ".zst"); /* main: .br */
    pick(all, "abrupt", NULL);                /* main: .br */
    pick(all, "GZIP", ".gz");                 /* main: none */
    pick(all, "x-gzip", ".gz");
    pick(all, "identity", NULL);
    pick(all, NULL, NULL);
    pick(gz,  "br, gzip", ".gz");             /* no .br on disk */
    pick(gz,  "br, zstd;q=0.5", ".zst");      /* main: .zst too */
    pick(gz,  "br;q=1, gzip;q=0", NULL);      /* main: .gz */
#ifndef C731_NO_CODING
    acc("br;q=0.5", "br", 1);
    acc("BR", "br", 1);
    acc("br;q=0", "br", 0);
    acc("brotli", "br", 0);
    acc("zstd;q=0, *", "zstd", 0);
    acc("*", "zstd", 1);
    acc("x-gzip", "gzip", 1);
    acc("x-gzip", "x-zstd", 0);
    acc("", "br", 0);
    acc(NULL, "br", 0);
    acc("gzip;q=0", "gzip", tk_gzip_accepted("gzip;q=0"));
    acc("gzip", "gzip", tk_gzip_accepted("gzip"));
#endif
    return bad ? 1 : 0;
}
EOF
mkdir -p "${WORK}/all" "${WORK}/gzonly"
for f in f.html f.html.br f.html.gz f.html.zst; do echo x > "${WORK}/all/${f}"; done
for f in f.html f.html.gz f.html.zst; do echo x > "${WORK}/gzonly/${f}"; done

EXTRA=()
grep -q 'tk_coding_accepted' "${S}/tk_gzip_type.h" 2>/dev/null || EXTRA=(-DC731_NO_CODING)
if ! ${CC} -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} ${EXTRA[@]+"${EXTRA[@]}"} -iquote "${S}" -I"${S}" \
        -o "${WORK}/h" "${WORK}/h.c" "${S}/content.c" -lz > "${WORK}/cc.log" 2>&1; then
    fail "the harness does not build: $(grep -m3 error "${WORK}/cc.log" | head -c 400)"
else
    "${WORK}/h" "${WORK}/all" "${WORK}/gzonly" > "${WORK}/out" 2> "${WORK}/err"
    rc=$?
    while IFS= read -r line; do
        case "${line}" in OK\ *) pass "${line#OK }" ;; BAD\ *) fail "${line#BAD }" ;; esac
    done < "${WORK}/out"
    if grep -a -q -E 'AddressSanitizer|runtime error' "${WORK}/err"; then
        fail "sanitizer: $(grep -a -m1 -E 'AddressSanitizer|runtime error' "${WORK}/err" | head -c 300)"
    elif [ "${rc}" -ne 0 ] && ! grep -q '^BAD ' "${WORK}/out"; then
        fail "the harness exited ${rc} with no verdict: $(head -c 300 "${WORK}/err")"
    fi
fi

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
