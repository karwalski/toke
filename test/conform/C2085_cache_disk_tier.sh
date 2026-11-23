#!/usr/bin/env bash
# C2085_cache_disk_tier.sh -- std.cache's disk tier spills what memory evicts
# and serves it back (story 127.496).
#
# THE DEFECT (integrate/wave-a-1122 ee27591c).  http_cache_put ran
# cache_evict() -- which frees LRU entries while the cache is over max_entries
# or max_size -- before its spill test `disk_dir && total_size > max_size`.
# After the eviction that test could only hold with no tail left, so the spill
# never ran: every evicted entry was dropped.  http_cache_get never looked in
# disk_dir either, so a spilled entry would not have been served, and
# disk_max_size was never consulted.  The disk tier of 63.1.6 was unreachable.
#
# THE FIX.  cache_evict() offers each victim to the disk tier before freeing
# it (within disk_max_size; a spill that would exceed it is not made).  The
# spill file stores the key, the insertion time and the lengths, so
# http_cache_get, on a memory miss, reads key's file back, checks it holds that
# key (FNV-1a filenames can collide), moves the entry to the front of memory
# and removes the file.  http_cache_put of a key drops its disk copy, and
# http_cache_purge / http_cache_purge_pattern purge matching disk files too.
#
# WHAT IS ASSERTED (cache.c is #included into a C driver that uses the public
# http_cache_* API; g_cache is read for counts.  Nothing in the tree calls
# http_cache_* yet, runtime-abi.md):
#   count  -- max_entries 2: the third put spills the LRU entry (file present,
#             disk_used = its body length); a get of it is a HIT with the same
#             status, body, etag, content type and insertion time, the file is
#             gone, and the entry it displaced is on disk in turn;
#   size   -- max_size 100: two 60-byte bodies spill the first; read back whole;
#   big    -- a body larger than max_size lives on disk only and is served;
#   diskmax-- disk_max_size 50: a 60-byte victim is not spilled (no file,
#             disk_used 0, later MISS); disk_max_size 0 spills nothing;
#   vary   -- a Vary-keyed entry is served back only for the same header value;
#   collide-- a file at the key's path that holds another key is not served;
#             a truncated file is a MISS;
#   reput  -- a put of a spilled key removes the disk copy and uncounts it;
#   purge  -- purge and purge_pattern remove matching spill files and count them;
#   crafted-- a file whose four lengths are huge and wrap when summed (the sum
#             equals the bytes left) is not an entry: a get of its key, a put
#             of its key and a purge all leave it unread.  The script also
#             builds the driver with -fsanitize=address,undefined (when the
#             compiler can) and runs this case there: before the per-length
#             check it was a heap-buffer-overflow read before the file buffer;
#   private-- a symlink planted at a key's spill path is not followed: the
#             spill replaces the link, its target is unchanged, and the spill
#             file is mode 0600.
#
# Negative control: C2085_STDLIB=<dir> builds against another tree's
# src/stdlib.  On ee27591c (and v3.2.0) the driver builds and the count, size,
# big, vary, reput and purge cases fail: nothing reaches disk_dir.
#
# Story: 127.496
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C2085_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2085_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2085: std.cache's disk tier spills evicted entries and reads them back (127.496)"
echo "--------------------------------------------------------------------------------"

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
static char *body_of(char c, size_t n)
{
    char *b = malloc(n + 1);
    memset(b, c, n); b[n] = 0;
    return b;
}
static void put(const char *uri, const char *body, size_t n, const char *etag)
{
    http_cache_put("GET", uri, 200, body, n, etag, NULL, NULL, "text/plain",
                   "max-age=300", NULL, NULL, 0);
}
static const char *get(const char *uri, const char **body, size_t *n,
                       const char **etag, const char **ct, uint32_t *st)
{
    *body = NULL; *n = 0; *etag = NULL; *ct = NULL; *st = 0;
    return http_cache_get("GET", uri, NULL, NULL, NULL, 0, st, body, n, etag, ct);
}

int main(int argc, char **argv)
{
    if (argc < 3) return 2;
    DIRP = argv[1];
    const char *c = argv[2];
    const char *b, *et, *ct; size_t n; uint32_t st;

    if (strcmp(c, "count") == 0) {
        http_cache_init(2, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        char *A = body_of('a', 40), *B = body_of('b', 30), *C = body_of('c', 20);
        time_t t0 = time(NULL);
        put("/a", A, 40, "\"ea\""); put("/b", B, 30, "\"eb\""); put("/c", C, 20, "\"ec\"");
        ok(g_cache.count == 2, "count: max_entries 2 holds two entries in memory");
        ok(on_disk("GET|/a"), "count: the LRU entry /a is spilled to disk_dir");
        ok(g_cache.disk_used == 40, "count: disk_used counts the spilled body (40)");
        const char *r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "HIT") == 0, "count: a get of the spilled /a is a HIT");
        ok(n == 40 && b && memcmp(b, A, 40) == 0, "count: the body read back is the stored body");
        ok(st == 200 && et && strcmp(et, "\"ea\"") == 0 && ct && strcmp(ct, "text/plain") == 0,
           "count: status, etag and content type are read back");
        CacheEntry *e = cache_find("GET|/a");
        ok(e && e->inserted >= t0 && e->inserted <= time(NULL) && e->ttl == 300,
           "count: insertion time and TTL are read back (freshness counts from the store)");
        ok(!on_disk("GET|/a"), "count: the read-back entry's file is removed");
        ok(on_disk("GET|/b") && g_cache.count == 2, "count: the entry it displaced (/b) is spilled in turn");
        ok(g_cache.disk_used == 30, "count: disk_used is /b's 30 bytes");
        r = get("/b", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "HIT") == 0 && n == 30 && memcmp(b, B, 30) == 0, "count: /b is served back too");
        r = get("/zz", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "MISS") == 0, "count: a key never stored is a MISS");
    } else if (strcmp(c, "size") == 0) {
        http_cache_init(100, 100);
        http_cache_set_disk(DIRP, 1 << 20);
        char *A = body_of('a', 60), *B = body_of('b', 60);
        put("/a", A, 60, NULL); put("/b", B, 60, NULL);
        ok(g_cache.count == 1 && g_cache.total_size == 60, "size: max_size 100 keeps one 60-byte body");
        ok(on_disk("GET|/a") && g_cache.disk_used == 60, "size: the other is spilled and counted");
        const char *r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "HIT") == 0 && n == 60 && memcmp(b, A, 60) == 0 && et == NULL,
           "size: read back whole, an absent etag stays absent");
    } else if (strcmp(c, "big") == 0) {
        http_cache_init(100, 100);
        http_cache_set_disk(DIRP, 1 << 20);
        char *A = body_of('x', 500);
        put("/big", A, 500, NULL);
        ok(g_cache.count == 0 && on_disk("GET|/big"), "big: a body over max_size goes straight to disk");
        const char *r = get("/big", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "HIT") == 0 && n == 500 && memcmp(b, A, 500) == 0,
           "big: it is served, and the returned body stays valid (not evicted under the caller)");
    } else if (strcmp(c, "diskmax") == 0) {
        http_cache_init(1, 1 << 20);
        http_cache_set_disk(DIRP, 50);
        char *A = body_of('a', 60), *B = body_of('b', 10), *C = body_of('c', 10);
        put("/a", A, 60, NULL); put("/b", B, 10, NULL);
        ok(!on_disk("GET|/a") && g_cache.disk_used == 0, "diskmax: a 60-byte victim over disk_max_size 50 is not spilled");
        const char *r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "MISS") == 0, "diskmax: it is a later MISS");
        put("/c", C, 10, NULL);
        ok(on_disk("GET|/b") && g_cache.disk_used == 10, "diskmax: a 10-byte victim within 50 is spilled");
        http_cache_set_disk(DIRP, 0);
        put("/d", C, 10, NULL);
        ok(!on_disk("GET|/c"), "diskmax: disk_max_size 0 spills nothing");
    } else if (strcmp(c, "vary") == 0) {
        http_cache_init(1, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        const char *hn[] = { "Accept-Encoding" };
        const char *gz[] = { "gzip" }, *br[] = { "br" };
        http_cache_put("GET", "/v", 200, "zipped", 6, NULL, NULL, "Accept-Encoding",
                       "text/plain", "max-age=300", hn, gz, 1);
        put("/other", "o", 1, NULL);
        ok(on_disk("GET|/v|Accept-Encoding=gzip"), "vary: the Vary-keyed entry is spilled under its full key");
        const char *r = http_cache_get("GET", "/v", "Accept-Encoding", hn, br, 1,
                                       &st, &b, &n, &et, &ct);
        ok(strcmp(r, "MISS") == 0, "vary: another Accept-Encoding is a MISS");
        r = http_cache_get("GET", "/v", "Accept-Encoding", hn, gz, 1, &st, &b, &n, &et, &ct);
        ok(strcmp(r, "HIT") == 0 && n == 6 && memcmp(b, "zipped", 6) == 0, "vary: the same Accept-Encoding is a HIT");
    } else if (strcmp(c, "collide") == 0) {
        http_cache_init(10, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        char p[1024]; path_of("GET|/a", p, sizeof(p));
        CacheEntry fake; memset(&fake, 0, sizeof(fake));
        fake.key = "GET|/not-a"; fake.status = 200; fake.ttl = 300;
        fake.inserted = time(NULL); fake.body = "other"; fake.body_len = 5;
        ok(cache_spill_file(p, &fake) == 0, "collide: a file for another key is placed at /a's path");
        const char *r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "MISS") == 0, "collide: it is not served for /a");
        ok(access(p, F_OK) == 0, "collide: and it is left in place");
        FILE *f = fopen(p, "wb"); fputs("TKC1\n200\n300\n0\n6 -1 -1 50\nGET|/ashort", f); fclose(f);
        r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "MISS") == 0, "collide: a truncated file is a MISS");
    } else if (strcmp(c, "reput") == 0) {
        http_cache_init(1, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        put("/a", "old", 3, NULL); put("/b", "bb", 2, NULL);
        ok(on_disk("GET|/a") && g_cache.disk_used == 3, "reput: /a is spilled");
        put("/a", "new!", 4, NULL);
        ok(g_cache.disk_used == 2 && on_disk("GET|/b"), "reput: the put of /a drops its disk copy (only /b's 2 bytes counted)");
        const char *r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "HIT") == 0 && n == 4 && memcmp(b, "new!", 4) == 0, "reput: the new body is served");
    } else if (strcmp(c, "purge") == 0) {
        http_cache_init(1, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        put("/api/x", "x", 1, NULL); put("/api/y", "yy", 2, NULL); put("/web", "w", 1, NULL);
        ok(on_disk("GET|/api/x") && on_disk("GET|/api/y"), "purge: /api/x and /api/y are spilled");
        ok(http_cache_purge("/api/x") == 1 && !on_disk("GET|/api/x"), "purge: purge(/api/x) removes its spill file and counts it");
        ok(g_cache.disk_used == 2, "purge: disk_used drops by its body");
        ok(http_cache_purge_pattern("/api/*") == 1 && !on_disk("GET|/api/y"), "purge: purge_pattern(/api/*) removes /api/y's file");
        const char *r = get("/api/y", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "MISS") == 0 && g_cache.disk_used == 0, "purge: a purged entry is a MISS, nothing counted");
    } else if (strcmp(c, "crafted") == 0) {
        http_cache_init(10, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        char p[1024]; path_of("GET|/a", p, sizeof(p));
        FILE *f = fopen(p, "wb");
        fputs("TKC1\n200\n300\n0\n9223372036854775799 9223372036854775599 228 0\n0123456789", f);
        fclose(f);
        const char *r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "MISS") == 0, "crafted: lengths that wrap when summed are a MISS, not a read outside the file");
        ok(http_cache_purge("/a") == 0 && access(p, F_OK) == 0, "crafted: purge does not take it for an entry");
        put("/a", "x", 1, NULL);
        r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "HIT") == 0 && n == 1 && memcmp(b, "x", 1) == 0, "crafted: a put of the key over it still works");
    } else if (strcmp(c, "private") == 0) {
        http_cache_init(1, 1 << 20);
        http_cache_set_disk(DIRP, 1 << 20);
        char p[1024]; path_of("GET|/a", p, sizeof(p));
        char tgt[1100]; snprintf(tgt, sizeof(tgt), "%s/../target_private", DIRP);
        FILE *f = fopen(tgt, "wb"); fputs("precious", f); fclose(f);
        ok(symlink(tgt, p) == 0, "private: a symlink is planted at /a's spill path");
        put("/a", "aaaa", 4, NULL); put("/b", "b", 1, NULL);
        char got[32] = {0};
        f = fopen(tgt, "rb"); size_t k = fread(got, 1, sizeof(got) - 1, f); fclose(f);
        ok(k == 8 && strcmp(got, "precious") == 0, "private: the link's target is not truncated or written");
        struct stat sb;
        ok(lstat(p, &sb) == 0 && S_ISREG(sb.st_mode) && (sb.st_mode & 0777) == 0600,
           "private: the spill file is a regular file, mode 0600");
        const char *r = get("/a", &b, &n, &et, &ct, &st);
        ok(strcmp(r, "HIT") == 0 && n == 4 && memcmp(b, "aaaa", 4) == 0, "private: and it is read back");
        unlink(tgt);
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
    echo "C2085: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
pass "the driver builds at -O1 -Werror with _FORTIFY_SOURCE"

run_case() { # drv case
    mkdir -p "d_${1}_${2}"
    "./${1}" "${WORK}/d_${1}_${2}" "${2}" > "run_${1}_${2}.log" 2>&1
}

for c in count size big diskmax vary collide reput purge crafted private; do
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

# The crafted file under ASan + UBSan: an out-of-bounds read aborts here even
# when the plain build happens not to fault.
mkdir -p probe
ASAN_CFLAGS=(-std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE
             -Wall -Wextra -Werror -Wno-unused-function -O0 -g -I"${S}"
             -fsanitize=address,undefined -fno-sanitize-recover=all)
if "${CC}" "${ASAN_CFLAGS[@]}" -o drv_asan driver.c > cc_asan.log 2>&1 &&
   ./drv_asan "${WORK}/probe" count > /dev/null 2>&1; then
    # allocator_may_return_null: a huge malloc returns NULL as it does in a
    # plain build, so the parse goes on to the read the overflow would make.
    for c in crafted collide; do
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

echo "C2085: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
