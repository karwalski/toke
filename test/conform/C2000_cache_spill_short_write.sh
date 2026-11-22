#!/usr/bin/env bash
# C2000_cache_spill_short_write.sh -- std.cache's disk spill writes the whole
# entry or leaves no file and counts nothing (story 127.445).
#
# THE DEFECT (integrate/wave-a-1122 0558545d).  http_cache_put's spill to
# disk_dir wrote the header with dprintf and the body with one write(2), and
# ignored both results, then added body_len to g_cache.disk_used.  A short
# write (ENOSPC, EFBIG, EINTR on a slow device) left a truncated .cache file
# and the byte accounting drifted.  Ubuntu's gcc also rejects the build at
# -O1 -Werror (write is warn_unused_result under _FORTIFY_SOURCE).
#
# THE FIX.  cache_spill_file() builds the header in memory and writes header
# and body through cache_write_all(), which retries on EINTR and continues
# after a short write.  A failed open, write or close removes the partial file,
# counts nothing and returns -1 with errno from the failing call.  The cache
# has no error channel (http_cache_put is void and is not on any toke
# surface): a failed spill leaves the entry evicted, a later MISS.
#
# WHAT IS ASSERTED (cache.c is #included into a C driver, so the static
# helpers are reachable; nothing in the tree calls http_cache_* yet, and the
# spill was not reachable through the API before 127.496; C2085 drives it so):
#   1. write(2) replaced by a shim (only cache.c's calls):
#        - every write returns at most 7 bytes and the first returns EINTR:
#          the file holds the whole header and body, disk_used grows by
#          body_len;
#        - writes fail with ENOSPC after 100 bytes: -1/ENOSPC, no file, the
#          previous spill of the same path is gone too (it was truncated by
#          open), disk_used unchanged;
#        - a write that returns 0: -1/ENOSPC, no file, nothing counted;
#   2. the real kernel: a child with RLIMIT_FSIZE of 512 bytes and SIGXFSZ
#      ignored spills a 4096-byte body: -1/EFBIG, no file, nothing counted;
#      a 64-byte body under the same limit is written whole and counted;
#   3. a read-only disk_dir: open fails, -1/EACCES, nothing counted (skipped
#      when run as root, which ignores the mode).
#
# Negative control: C2000_STDLIB=<dir> builds against another tree's
# src/stdlib.  On 0558545d (and v3.2.0) there is no cache_spill_file: the
# harness reports the old tree and fails.  The old inline code, transplanted
# into a function in a scratch driver, under the same RLIMIT_FSIZE child left
# a 512-byte truncated file and counted all 4096 bytes (Z229).
#
# The file layout checked by whole_file() is the one 127.496 introduced (key,
# inserted time and lengths in the header); C2085 covers the tier itself.
#
# Story: 127.445
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C2000_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2000_XXXXXX")"
trap 'chmod -R u+w "${WORK}" 2>/dev/null; rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2000: std.cache's disk spill handles short and failed writes (127.445)"
echo "----------------------------------------------------------------------"

if ! grep -q 'cache_spill_file' "${S}/cache.c"; then
    fail "no cache_spill_file in ${S}/cache.c: the spill ignores write(2)'s result (old tree)"
    echo "C2000: ${PASS} passed, ${FAIL} failed"
    exit 1
fi

cat > driver.c <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/resource.h>

#ifdef C2000_SHIM
/* Only cache.c's write calls go through the shim. */
static int    shim_cap = 0;        /* max bytes per call, 0 = no cap */
static int    shim_eintr = 0;      /* fail the next N calls with EINTR */
static long   shim_budget = -1;    /* bytes before ENOSPC, -1 = no limit */
static int    shim_zero = 0;       /* return 0 instead of writing */
static long   shim_calls = 0;
static ssize_t shim_write(int fd, const void *buf, size_t len)
{
    shim_calls++;
    if (shim_eintr > 0) { shim_eintr--; errno = EINTR; return -1; }
    if (shim_zero) return 0;
    if (shim_cap > 0 && len > (size_t)shim_cap) len = (size_t)shim_cap;
    if (shim_budget >= 0) {
        if (shim_budget == 0) { errno = ENOSPC; return -1; }
        if ((long)len > shim_budget) len = (size_t)shim_budget;
        shim_budget -= (long)len;
    }
    return write(fd, buf, len);
}
#define write shim_write
#endif
#include "cache.c"
#undef write

static int fails = 0;
static void ok(int c, const char *what)
{
    printf("  %s: %s\n", c ? "PASS" : "FAIL", what);
    if (!c) fails++;
}

static CacheEntry mk(size_t blen)
{
    CacheEntry e;
    memset(&e, 0, sizeof(e));
    e.status = 200; e.ttl = 60;
    e.etag = "\"abc\""; e.content_type = "text/plain";
    e.body = malloc(blen + 1);
    for (size_t i = 0; i < blen; i++) e.body[i] = (char)('a' + (int)(i % 26));
    e.body[blen] = 0;
    e.body_len = blen;
    return e;
}

static long fsize(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 ? (long)st.st_size : -1;
}

/* The 127.496 layout: header, then key (empty here), etag, type, body. */
static int whole_file(const char *p, const CacheEntry *e)
{
    char HDR[128];
    size_t hl = (size_t)snprintf(HDR, sizeof(HDR),
        "TKC1\n200\n60\n0\n0 5 10 %zu\n\"abc\"text/plain", e->body_len);
    if (fsize(p) != (long)(hl + e->body_len)) return 0;
    FILE *f = fopen(p, "rb");
    if (!f) return 0;
    char *buf = malloc(hl + e->body_len);
    size_t got = fread(buf, 1, hl + e->body_len, f);
    fclose(f);
    int r = got == hl + e->body_len && memcmp(buf, HDR, hl) == 0 &&
            memcmp(buf + hl, e->body, e->body_len) == 0;
    free(buf);
    return r;
}

int main(int argc, char **argv)
{
    const char *dir = argv[1];
    char path[1024];
    snprintf(path, sizeof(path), "%s/e.cache", dir);
    (void)argc;
#ifdef C2000_SHIM
    CacheEntry e = mk(4096);
    g_cache.disk_used = 0;
    shim_cap = 7; shim_eintr = 1; shim_budget = -1; shim_calls = 0;
    int rc = cache_spill_file(path, &e);
    ok(rc == 0, "7-byte short writes and one EINTR: the spill returns 0");
    ok(whole_file(path, &e), "7-byte short writes and one EINTR: the file holds the whole header and body");
    ok(g_cache.disk_used == 4096, "7-byte short writes and one EINTR: disk_used counts body_len");
    ok(shim_calls > 4096 / 7, "the shim saw the short writes (the loop ran)");

    shim_cap = 0; shim_eintr = 0; shim_budget = 100;
    errno = 0;
    rc = cache_spill_file(path, &e);
    int er = errno;
    ok(rc == -1 && er == ENOSPC, "ENOSPC after 100 bytes: the spill returns -1 with errno ENOSPC");
    ok(access(path, F_OK) != 0, "ENOSPC after 100 bytes: the partial file is removed");
    ok(g_cache.disk_used == 4096, "ENOSPC after 100 bytes: disk_used is unchanged");

    shim_budget = -1; shim_zero = 1;
    errno = 0;
    rc = cache_spill_file(path, &e);
    er = errno;
    ok(rc == -1 && er == ENOSPC, "a write returning 0: -1 with errno ENOSPC, not an endless loop");
    ok(access(path, F_OK) != 0, "a write returning 0: no file is left");
    ok(g_cache.disk_used == 4096, "a write returning 0: disk_used is unchanged");
    shim_zero = 0;
#else
    /* Real kernel: RLIMIT_FSIZE in a child. */
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl = { 512, 512 };
        if (setrlimit(RLIMIT_FSIZE, &rl) != 0) _exit(90);
        CacheEntry big = mk(4096);
        g_cache.disk_used = 0;
        errno = 0;
        int rc = cache_spill_file(path, &big);
        int er = errno;
        int code = 0;
        if (!(rc == -1 && er == EFBIG)) code |= 1;
        if (access(path, F_OK) == 0) code |= 2;
        if (g_cache.disk_used != 0) code |= 4;
        CacheEntry small = mk(64);
        rc = cache_spill_file(path, &small);
        if (!(rc == 0 && whole_file(path, &small) && g_cache.disk_used == 64)) code |= 8;
        _exit(code);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    int code = WIFEXITED(st) ? WEXITSTATUS(st) : 255;
    ok(code != 90, "RLIMIT_FSIZE could be set in the child");
    ok(code != 255, "the RLIMIT_FSIZE child exited normally (no SIGXFSZ)");
    ok(code != 90 && code != 255 && !(code & 1), "RLIMIT_FSIZE 512, 4096-byte body: -1 with errno EFBIG");
    ok(code != 90 && code != 255 && !(code & 2), "RLIMIT_FSIZE 512, 4096-byte body: no truncated file is left");
    ok(code != 90 && code != 255 && !(code & 4), "RLIMIT_FSIZE 512, 4096-byte body: nothing is counted");
    ok(code != 90 && code != 255 && !(code & 8), "RLIMIT_FSIZE 512, 64-byte body: written whole and counted");

    if (geteuid() != 0) {
        char ro[1024];
        snprintf(ro, sizeof(ro), "%s/ro", dir);
        mkdir(ro, 0555);
        chmod(ro, 0555);
        snprintf(path, sizeof(path), "%s/e.cache", ro);
        CacheEntry e = mk(100);
        g_cache.disk_used = 7;
        errno = 0;
        int rc = cache_spill_file(path, &e);
        int er = errno;
        ok(rc == -1 && er == EACCES, "read-only disk_dir: -1 with errno EACCES");
        ok(access(path, F_OK) != 0, "read-only disk_dir: no file");
        ok(g_cache.disk_used == 7, "read-only disk_dir: disk_used is unchanged");
    } else {
        printf("  SKIP: read-only disk_dir (running as root)\n");
    }
#endif
    return fails ? 1 : 0;
}
EOF

CFLAGS=(-std=c99 -D_POSIX_C_SOURCE=200809L -D_DARWIN_C_SOURCE -D_DEFAULT_SOURCE
        -Wall -Wextra -Werror -Wno-unused-function -O1 -D_FORTIFY_SOURCE=2 -I"${S}")
for v in shim kernel; do
    def=(); [ "$v" = shim ] && def=(-DC2000_SHIM)
    if ! "${CC}" "${CFLAGS[@]}" ${def[@]+"${def[@]}"} -o "drv_${v}" driver.c > "cc_${v}.log" 2>&1; then
        fail "the ${v} driver builds at -O1 -Werror with _FORTIFY_SOURCE"
        sed 's/^/    /' "cc_${v}.log" | head -20
        continue
    fi
    pass "the ${v} driver builds at -O1 -Werror with _FORTIFY_SOURCE"
    mkdir -p "d_${v}"
    ./"drv_${v}" "${WORK}/d_${v}" > "run_${v}.log" 2>&1
    while IFS= read -r line; do
        case "$line" in
            "  PASS: "*) pass "${line#  PASS: }" ;;
            "  FAIL: "*) fail "${line#  FAIL: }" ;;
            "  SKIP: "*) echo "$line" ;;
            *) echo "    $line" ;;
        esac
    done < "run_${v}.log"
done

echo "C2000: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
