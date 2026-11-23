#!/usr/bin/env bash
# C2160_cache_long_key.sh -- std.cache keys are built to their full length:
# no heap overflow past 1024 bytes, and no two long URIs sharing an entry
# (story 127.516).
#
# THE DEFECT (integrate/wave-f-1123 5a65c2e9).  cache_build_key malloc'd 1024
# bytes and wrote "METHOD|URI" with snprintf, keeping snprintf's return (the
# untruncated length) as the offset.  For a URI of ~1020+ bytes the offset was
# past the buffer, and a matched Vary header was then appended with
# snprintf(key + off, cap - off, ...): a write past the block with a wrapped
# size_t bound (heap-buffer-overflow under ASan).  Without Vary the key was cut
# at 1023 bytes, so two URIs sharing their first ~1019 bytes shared one entry
# for get, put and purge; http_cache_stale_if_error cut its "METHOD|URI" the
# same way in a 1024-byte stack buffer.
#
# THE FIX.  The key (and the stale-if-error prefix) grows to fit every append,
# so a key is the exact bytes of METHOD|URI|vary... at any length; matching
# stays exact (127.514: the byte after METHOD|URI ends the key or is '|').
#
# WHAT IS ASSERTED (cache.c is #included into a C driver that uses the public
# http_cache_* API, as C2150 does; nothing in the tree calls http_cache_* yet):
#   vary  -- get and put of a 1101-byte URI with a matching Vary header do not
#            overrun; the entry is a HIT for the same header value and a MISS
#            for another;
#   long  -- two 1500-byte URIs that differ only in their last byte are two
#            entries: each get, and each stale_if_error, serves its own body,
#            and purging one leaves the other;
#   disk  -- a 1500-byte key spilled to disk is read back as that key, and
#            its 1500-byte sibling is a MISS.
#   Every case also runs under ASan + UBSan when the compiler can.
#
# Negative control: C2160_STDLIB=<dir> builds against another tree's
# src/stdlib.  On 5a65c2e9 the long and disk cases fail and the vary case
# aborts under ASan (heap-buffer-overflow in snprintf).
#
# Story: 127.516
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C2160_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2160_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2160: std.cache builds keys to their full length (127.516)"
echo "------------------------------------------------------------"

cat > driver.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "cache.c"

static int fails = 0;
static void ok(int c, const char *what)
{
    printf("  %s: %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}

/* "/" + n-2 'x' + last */
static char *long_uri(size_t n, char last)
{
    char *u = malloc(n + 1);
    u[0] = '/';
    memset(u + 1, 'x', n - 2);
    u[n - 1] = last;
    u[n] = '\0';
    return u;
}
static void put(const char *uri, const char *body, const char *vary,
                const char *const *hn, const char *const *hv, int nh)
{
    http_cache_put("GET", uri, 200, body, strlen(body), NULL, NULL, vary,
                   "text/plain", "max-age=300", hn, hv, nh);
}
static const char *get(const char *uri, const char *vary, const char *const *hn,
                       const char *const *hv, int nh, const char **body, size_t *n)
{
    uint32_t st = 0; const char *et = NULL, *ct = NULL;
    *body = NULL; *n = 0;
    return http_cache_get("GET", uri, vary, hn, hv, nh, &st, body, n, &et, &ct);
}
static int is(const char *b, size_t n, const char *want)
{
    return b && n == strlen(want) && memcmp(b, want, n) == 0;
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    const char *dir = argv[1], *c = argv[2];
    const char *b; size_t n; const char *r;

    if (strcmp(c, "vary") == 0) {
        http_cache_init(10, 1 << 20);
        char *u = long_uri(1101, 'v');
        const char *hn[] = { "Accept-Encoding" };
        const char *br[] = { "br" }, *gz[] = { "gzip" };
        r = get(u, "Accept-Encoding", hn, br, 1, &b, &n);
        ok(strcmp(r, "MISS") == 0, "vary: get of a 1101-byte URI with a matched Vary header is a MISS");
        put(u, "br-body", "Accept-Encoding", hn, br, 1);
        r = get(u, "Accept-Encoding", hn, br, 1, &b, &n);
        ok(strcmp(r, "HIT") == 0 && is(b, n, "br-body"), "vary: the same header value is a HIT");
        r = get(u, "Accept-Encoding", hn, gz, 1, &b, &n);
        ok(strcmp(r, "MISS") == 0, "vary: another header value is a MISS");
        free(u);
    } else if (strcmp(c, "long") == 0) {
        http_cache_init(10, 1 << 20);
        char *a = long_uri(1500, 'a'), *z = long_uri(1500, 'z');
        put(a, "a-body", NULL, NULL, NULL, 0);
        put(z, "z-body", NULL, NULL, NULL, 0);
        ok(http_cache_count() == 2, "long: two 1500-byte URIs differing in the last byte are two entries");
        r = get(a, NULL, NULL, NULL, 0, &b, &n);
        ok(strcmp(r, "HIT") == 0 && is(b, n, "a-body"), "long: get of the first serves its own body");
        r = get(z, NULL, NULL, NULL, 0, &b, &n);
        ok(strcmp(r, "HIT") == 0 && is(b, n, "z-body"), "long: get of the second serves its own body");
        uint32_t st = 0; const char *ct = NULL;
        b = http_cache_stale_if_error("GET", a, &st, &n, &ct);
        ok(is(b, n, "a-body"), "long: stale_if_error of the first serves its own body");
        b = http_cache_stale_if_error("GET", z, &st, &n, &ct);
        ok(is(b, n, "z-body"), "long: stale_if_error of the second serves its own body");
        ok(http_cache_purge(a) == 1, "long: purging the first removes one entry");
        r = get(a, NULL, NULL, NULL, 0, &b, &n);
        ok(strcmp(r, "MISS") == 0, "long: the first is then a MISS");
        r = get(z, NULL, NULL, NULL, 0, &b, &n);
        ok(strcmp(r, "HIT") == 0 && is(b, n, "z-body"), "long: the second is still a HIT");
        free(a); free(z);
    } else if (strcmp(c, "disk") == 0) {
        http_cache_init(1, 1 << 20);
        http_cache_set_disk(dir, 1 << 20);
        char *a = long_uri(1500, 'a'), *z = long_uri(1500, 'z');
        put(a, "a-body", NULL, NULL, NULL, 0);
        put("/other", "other-body", NULL, NULL, NULL, 0); /* spills a */
        r = get(z, NULL, NULL, NULL, 0, &b, &n);
        ok(strcmp(r, "MISS") == 0, "disk: the 1500-byte sibling of a spilled key is a MISS");
        r = get(a, NULL, NULL, NULL, 0, &b, &n);
        ok(strcmp(r, "HIT") == 0 && is(b, n, "a-body"), "disk: the spilled 1500-byte key reads back as itself");
        free(a); free(z);
    } else {
        return 2;
    }
    return fails ? 1 : 0;
}
EOF

CFLAGS=(-std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE
        -Wall -Wextra -Werror -Wno-unused-function -O1 -D_FORTIFY_SOURCE=2 -I"${S}")
if ! "${CC}" "${CFLAGS[@]}" -o drv driver.c > cc.log 2>&1; then
    fail "the driver builds at -O1 -Werror with _FORTIFY_SOURCE"
    sed 's/^/    /' cc.log | head -20
    echo "C2160: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
pass "the driver builds at -O1 -Werror with _FORTIFY_SOURCE"

CASES="vary long disk"
run_case() { # drv case
    mkdir -p "d_${1}_${2}"
    "./${1}" "${WORK}/d_${1}_${2}" "${2}" > "run_${1}_${2}.log" 2>&1
}

for c in ${CASES}; do
    run_case drv "${c}"
    rc=$?
    while IFS= read -r line; do
        case "$line" in
            "  PASS: "*) pass "${line#  PASS: }" ;;
            "  FAIL: "*) fail "${line#  FAIL: }" ;;
            *) echo "    $line" ;;
        esac
    done < "run_drv_${c}.log"
    if [ "$rc" -gt 1 ]; then fail "${c}: the driver exited ${rc}"; fi
done

mkdir -p probe
ASAN_CFLAGS=(-std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE
             -Wall -Wextra -Werror -Wno-unused-function -O0 -g -I"${S}"
             -fsanitize=address,undefined -fno-sanitize-recover=all)
cat > probe.c <<'EOF'
int main(void) { return 0; }
EOF
if "${CC}" "${ASAN_CFLAGS[@]}" -o probe_asan probe.c > /dev/null 2>&1 &&
   ./probe_asan > /dev/null 2>&1 &&
   "${CC}" "${ASAN_CFLAGS[@]}" -o drv_asan driver.c > cc_asan.log 2>&1; then
    for c in ${CASES}; do
        run_case drv_asan "${c}"
        rc=$?
        if [ "$rc" -eq 0 ]; then
            pass "${c}: clean under ASan + UBSan"
        else
            fail "${c}: under ASan + UBSan (exit ${rc})"
            grep -E 'ERROR|runtime error|FAIL' "run_drv_asan_${c}.log" | head -5 | sed 's/^/    /'
        fi
    done
else
    echo "  SKIP: the compiler cannot build or run an ASan + UBSan driver here"
fi

echo "C2160: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
