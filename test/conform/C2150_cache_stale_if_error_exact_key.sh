#!/usr/bin/env bash
# C2150_cache_stale_if_error_exact_key.sh -- std.cache's stale-if-error
# fallback matches the request's own key, not any key that starts with it
# (story 127.514).
#
# THE DEFECT (integrate/wave-a-1122 05010ab8).  http_cache_stale_if_error built
# "METHOD|URI" and accepted any entry whose key began with those bytes
# (strncmp over the prefix length), so stale_if_error(GET, /a) served a cached
# GET /ab, /a/b or /api body when /a itself was not cached.  Since 127.511 the
# disk-tier scan uses the same matcher, so a spilled /ab was offered too.
#
# THE FIX.  An entry matches only when its key is "METHOD|URI" itself or a
# Vary variant of it, "METHOD|URI|...": the byte after the prefix must end the
# key or be '|', the separator cache_key_names (purge) already splits on.
# (Keys longer than 1023 bytes are cut by cache_build_key for get and put
# alike; that is story 127.516, not this one.)
#
# WHAT IS ASSERTED (cache.c is #included into a C driver that uses the public
# http_cache_* API, as C2130 does; nothing in the tree calls http_cache_* yet):
#   memory -- /ab, /a/b and /api cached, /a not: stale_if_error(GET, /a) is
#             NULL; once /a is put, its own body is served;
#   disk   -- a spilled GET|/ab is not the fallback for /a and stays on disk;
#             a spilled Vary variant GET|/v|Accept-Encoding=br is the fallback
#             for /v.
#   The script also runs every case under ASan + UBSan when it can.
#
# Negative control: C2150_STDLIB=<dir> builds against another tree's
# src/stdlib.  On 05010ab8 the memory and disk "not the fallback" assertions fail.
#
# Story: 127.514
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C2150_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2150_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2150: std.cache stale-if-error matches the exact key (127.514)"
echo "---------------------------------------------------------------"

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

static const char *DIRP;

static void path_of(const char *key, char *out, size_t cap)
{
    uint32_t h = 2166136261u;
    for (const char *p = key; *p; p++) { h ^= (uint8_t)*p; h *= 16777619u; }
    snprintf(out, cap, "%s/%08x.cache", DIRP, h);
}
static int on_disk(const char *key)
{
    char p[1024]; path_of(key, p, sizeof(p));
    return access(p, F_OK) == 0;
}
static void put(const char *m, const char *uri, const char *body)
{
    http_cache_put(m, uri, 200, body, strlen(body), NULL, NULL, NULL, "text/plain",
                   "max-age=300", NULL, NULL, 0);
}
static int plant(const char *key, const char *body)
{
    char p[1024]; path_of(key, p, sizeof(p));
    CacheEntry f; memset(&f, 0, sizeof(f));
    f.key = (char *)key; f.status = 203; f.ttl = 300;
    f.inserted = time(NULL); f.body = (char *)body;
    f.body_len = strlen(body); f.content_type = "text/plain";
    return cache_spill_file(p, &f);
}
static const char *stale(const char *uri, size_t *n)
{
    uint32_t st = 0; const char *ct = NULL; *n = 0;
    return http_cache_stale_if_error("GET", uri, &st, n, &ct);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    DIRP = argv[1];
    const char *c = argv[2];
    const char *b; size_t n;

    if (strcmp(c, "memory") == 0) {
        http_cache_init(10, 1 << 20);
        put("GET", "/ab", "ab-body");
        put("GET", "/a/b", "slash-body");
        put("GET", "/api", "api-body");
        b = stale("/a", &n);
        ok(b == NULL, "memory: /ab, /a/b, /api cached, /a not: no fallback for /a");
        put("GET", "/a", "a-body");
        b = stale("/a", &n);
        ok(b && n == 6 && memcmp(b, "a-body", 6) == 0, "memory: /a's own body once it is cached");
        b = stale("/ab", &n);
        ok(b && n == 7 && memcmp(b, "ab-body", 7) == 0, "memory: /ab still serves /ab");
    } else if (strcmp(c, "disk") == 0) {
        http_cache_init(10, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        ok(plant("GET|/ab", "ab-body") == 0 &&
           plant("GET|/v|Accept-Encoding=br", "vary-body") == 0, "disk: spill files planted");
        b = stale("/a", &n);
        ok(b == NULL, "disk: a spilled /ab is not the fallback for /a");
        ok(on_disk("GET|/ab") && cache_find("GET|/ab") == NULL, "disk: and it is left on disk");
        b = stale("/v", &n);
        ok(b && n == 9 && memcmp(b, "vary-body", 9) == 0,
           "disk: a Vary variant of /v is the fallback for /v");
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
    echo "C2150: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
pass "the driver builds at -O1 -Werror with _FORTIFY_SOURCE"

run_case() { # drv case
    mkdir -p "d_${1}_${2}"
    "./${1}" "${WORK}/d_${1}_${2}" "${2}" > "run_${1}_${2}.log" 2>&1
}

for c in memory disk; do
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
if "${CC}" "${ASAN_CFLAGS[@]}" -o drv_asan driver.c > cc_asan.log 2>&1 &&
   ./drv_asan "${WORK}/probe" memory > /dev/null 2>&1; then
    for c in memory disk; do
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

echo "C2150: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
