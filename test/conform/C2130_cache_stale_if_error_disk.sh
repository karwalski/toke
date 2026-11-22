#!/usr/bin/env bash
# C2130_cache_stale_if_error_disk.sh -- std.cache's stale-if-error fallback
# reads the disk tier too (story 127.511).
#
# THE DEFECT (integrate/wave-f-1123 6ec0044d).  127.496 made the disk tier
# reachable: an entry evicted from memory is spilled to disk_dir and
# http_cache_get reads it back.  http_cache_stale_if_error still scanned only
# the in-memory list by its "METHOD|URI" prefix, so an entry that had been
# spilled was never offered as the stale fallback: the origin errors, the
# copy is on disk, and the caller gets NULL.
#
# THE FIX.  When no memory entry matches, http_cache_stale_if_error scans
# disk_dir the way cache_disk_purge does (only <8 hex>.cache names), loads each
# file through cache_disk_load -- 127.496's read-back, which checks every
# length against the bytes left -- and takes the matching file with the
# latest insertion time that is inside the same window as the memory tier
# (TTL + 300 s).  That entry moves to the front of memory (its file removed
# and uncounted, as a get's read-back does), and its body is returned.
#
# WHAT IS ASSERTED (cache.c is #included into a C driver that uses the public
# http_cache_* API; g_cache is read for counts.  Nothing in the tree calls
# http_cache_* yet, runtime-abi.md):
#   spilled -- max_entries 1: /a is spilled by /b's put; stale_if_error(GET,
#              /a) returns /a's body, status and content type; the file is
#              removed and uncounted, /a is in memory and /b is spilled;
#   window  -- a spill file inserted 400 s ago with TTL 300 is served (inside
#              TTL + 300); one inserted 1000 s ago is not, and is left on disk;
#   memfirst-- a matching memory entry wins and the matching disk file is left
#              alone;
#   newest  -- of two matching Vary variants on disk the later insertion is
#              served;
#   method  -- a spilled POST entry is not the fallback for GET; no disk_dir
#              set: NULL as before;
#   crafted -- a file whose four lengths wrap when summed, and a truncated
#              file, are not entries: NULL, both left in place.  The script
#              also builds the driver with -fsanitize=address,undefined (when
#              the compiler can) and runs crafted and spilled there.
#
# Negative control: C2130_STDLIB=<dir> builds against another tree's
# src/stdlib.  On 6ec0044d the driver builds and the spilled, window and
# newest cases fail: nothing on disk is offered.
#
# Story: 127.511
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C2130_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2130_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2130: std.cache stale-if-error falls back to the disk tier (127.511)"
echo "---------------------------------------------------------------------"

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

/* The driver's own FNV-1a path, so it builds against the old tree too. */
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
static void put(const char *m, const char *uri, const char *body, const char *ct)
{
    http_cache_put(m, uri, 200, body, strlen(body), NULL, NULL, NULL, ct,
                   "max-age=300", NULL, NULL, 0);
}
/* Place a spill file for key, inserted ago seconds back, with ttl. */
static int plant(const char *key, const char *body, long ttl, long ago)
{
    char p[1024]; path_of(key, p, sizeof(p));
    CacheEntry f; memset(&f, 0, sizeof(f));
    f.key = (char *)key; f.status = 203; f.ttl = ttl;
    f.inserted = time(NULL) - ago; f.body = (char *)body;
    f.body_len = strlen(body); f.content_type = "text/plain";
    return cache_spill_file(p, &f);
}
static const char *stale(const char *m, const char *uri, uint32_t *st,
                         size_t *n, const char **ct)
{
    *st = 0; *n = 0; *ct = NULL;
    return http_cache_stale_if_error(m, uri, st, n, ct);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    DIRP = argv[1];
    const char *c = argv[2];
    const char *b, *ct; size_t n; uint32_t st;

    if (strcmp(c, "spilled") == 0) {
        http_cache_init(1, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        put("GET", "/a", "alpha-body", "text/html");
        put("GET", "/b", "bb", "text/plain");
        ok(on_disk("GET|/a") && g_cache.count == 1, "spilled: /a is on disk, /b in memory");
        b = stale("GET", "/a", &st, &n, &ct);
        ok(b && n == 10 && memcmp(b, "alpha-body", 10) == 0,
           "spilled: stale_if_error(GET, /a) serves the spilled body");
        ok(st == 200 && ct && strcmp(ct, "text/html") == 0,
           "spilled: with its status and content type");
        ok(!on_disk("GET|/a") && cache_find("GET|/a") != NULL,
           "spilled: the entry moved to memory and its file is removed");
        ok(on_disk("GET|/b") && g_cache.count == 1 && g_cache.disk_used == 2,
           "spilled: /b is spilled in its place, disk_used counts only /b");
        b = stale("GET", "/a", &st, &n, &ct);
        ok(b && n == 10, "spilled: a second call finds it in memory");
    } else if (strcmp(c, "window") == 0) {
        http_cache_init(10, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        ok(plant("GET|/w", "within", 300, 400) == 0 &&
           plant("GET|/old", "too-old", 300, 1000) == 0, "window: two spill files planted");
        b = stale("GET", "/w", &st, &n, &ct);
        ok(b && n == 6 && memcmp(b, "within", 6) == 0 && st == 203,
           "window: expired 100 s past its TTL (inside TTL + 300) is served");
        b = stale("GET", "/old", &st, &n, &ct);
        ok(b == NULL, "window: 700 s past its TTL is not served");
        ok(on_disk("GET|/old") && cache_find("GET|/old") == NULL,
           "window: and it is left on disk, not moved to memory");
    } else if (strcmp(c, "memfirst") == 0) {
        http_cache_init(10, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        ok(plant("GET|/m|Accept-Encoding=br", "disk", 300, 0) == 0, "memfirst: a disk variant planted");
        put("GET", "/m", "memory", "text/plain");
        b = stale("GET", "/m", &st, &n, &ct);
        ok(b && n == 6 && memcmp(b, "memory", 6) == 0, "memfirst: the memory entry is served");
        ok(on_disk("GET|/m|Accept-Encoding=br"), "memfirst: the disk file is left alone");
    } else if (strcmp(c, "newest") == 0) {
        http_cache_init(10, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        ok(plant("GET|/n|Accept-Encoding=gzip", "older", 300, 50) == 0 &&
           plant("GET|/n|Accept-Encoding=br", "newer", 300, 5) == 0 &&
           plant("GET|/n|Accept-Encoding=x", "oldest", 300, 90) == 0,
           "newest: three disk variants planted");
        b = stale("GET", "/n", &st, &n, &ct);
        ok(b && n == 5 && memcmp(b, "newer", 5) == 0, "newest: the latest insertion is served");
        ok(on_disk("GET|/n|Accept-Encoding=gzip") && on_disk("GET|/n|Accept-Encoding=x") &&
           !on_disk("GET|/n|Accept-Encoding=br"), "newest: only the served file is taken");
    } else if (strcmp(c, "method") == 0) {
        http_cache_init(1, 1 << 20);
        b = stale("GET", "/p", &st, &n, &ct);
        ok(b == NULL, "method: no disk_dir and nothing cached: NULL");
        http_cache_set_disk(DIRP, 1 << 20);
        put("POST", "/p", "posted", "text/plain");
        put("GET", "/q", "q", "text/plain");
        ok(on_disk("POST|/p"), "method: the POST entry is spilled");
        b = stale("GET", "/p", &st, &n, &ct);
        ok(b == NULL && on_disk("POST|/p"), "method: it is not the fallback for GET /p");
        b = stale("POST", "/p", &st, &n, &ct);
        ok(b && n == 6, "method: it is the fallback for POST /p");
    } else if (strcmp(c, "crafted") == 0) {
        http_cache_init(10, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        char p[1024]; path_of("GET|/a", p, sizeof(p));
        FILE *f = fopen(p, "wb");
        fputs("TKC1\n200\n300\n0\n9223372036854775799 9223372036854775599 228 0\n0123456789", f);
        fclose(f);
        char q[1024]; path_of("GET|/t", q, sizeof(q));
        f = fopen(q, "wb"); fputs("TKC1\n200\n300\n0\n6 -1 -1 50\nGET|/tshort", f); fclose(f);
        b = stale("GET", "/a", &st, &n, &ct);
        ok(b == NULL, "crafted: lengths that wrap when summed are not a fallback");
        b = stale("GET", "/t", &st, &n, &ct);
        ok(b == NULL, "crafted: a truncated file is not a fallback");
        ok(access(p, F_OK) == 0 && access(q, F_OK) == 0, "crafted: both files are left in place");
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
    echo "C2130: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
pass "the driver builds at -O1 -Werror with _FORTIFY_SOURCE"

run_case() { # drv case
    mkdir -p "d_${1}_${2}"
    "./${1}" "${WORK}/d_${1}_${2}" "${2}" > "run_${1}_${2}.log" 2>&1
}

for c in spilled window memfirst newest method crafted; do
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
   ./drv_asan "${WORK}/probe" memfirst > /dev/null 2>&1; then
    for c in crafted spilled newest; do
        ASAN_OPTIONS=allocator_may_return_null=1 run_case drv_asan "${c}"
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

echo "C2130: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
