#!/usr/bin/env bash
# C205_log_rotation_keeps_source_on_gz_failure.sh — access-log rotation never
# deletes a rotated log whose .gz did not finish (story 135.3j (5)).
#
# THE DEFECT (measured on main before 135.3j).  compress_to_gz() ignored the
# result of gzclose().  For a small file gzwrite() only buffers, so gzclose()
# is what writes the compressed data and the gzip trailer; when that write
# fails (disk full, a file-size limit, EIO) the function still reported
# success and unlink()ed the source: the rotated log was gone and the .gz
# left behind was truncated.
#
# WHAT THIS DRIVES: compress_to_gz() (log.c is #included, the function is
# static) under RLIMIT_FSIZE, with SIGXFSZ ignored, so the .gz cannot grow
# past 64 bytes (in a child, so the limit cannot touch the report).  4000 bytes of incompressible data stay in gzwrite's buffer
# and the failure lands in gzclose().  Expected: -1, the source still there
# byte-for-byte, no partial .gz.  Control: the same call with no limit
# succeeds, removes the source, and the .gz inflates to the original.
#
# Negative control: C205_LOG_C=<file> builds against another log.c; main's
# returns 0 and deletes the source.
#
# Story: 135.3j

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
LOG_C="${C205_LOG_C:-${S}/log.c}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c205_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C205: a failed gzclose() keeps the rotated log"
echo "----------------------------------------------"

cat > "${WORK}/t.c" <<C
#include "${LOG_C}"
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
static int fails = 0, checks = 0;
#define CHECK(c, m) do { checks++; if (c) printf("  PASS: %s\n", m); \\
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
static unsigned char data[4000];
static void mk(const char *p) { FILE *f = fopen(p, "wb"); fwrite(data, 1, sizeof data, f); fclose(f); }
static int same(const char *p) {
    unsigned char b[5000]; FILE *f = fopen(p, "rb"); if (!f) return 0;
    size_t n = fread(b, 1, sizeof b, f); fclose(f);
    return n == sizeof data && memcmp(b, data, n) == 0;
}
int main(int argc, char **argv) {
    (void)argc;
    setvbuf(stdout, NULL, _IONBF, 0);
    char src[1024], dst[1024];
    unsigned x = 12345;
    for (size_t i = 0; i < sizeof data; i++) { x = x * 1103515245u + 12345u; data[i] = (unsigned char)(x >> 16); }

    snprintf(src, sizeof src, "%s/ok.log", argv[1]); snprintf(dst, sizeof dst, "%s/ok.log.gz", argv[1]);
    mk(src);
    int rc = compress_to_gz(src, dst);
    gzFile g = gzopen(dst, "rb"); unsigned char back[5000]; int n = g ? gzread(g, back, sizeof back) : -1;
    if (g) gzclose(g);
    CHECK(rc == 0 && access(src, F_OK) != 0 && n == (int)sizeof data && !memcmp(back, data, sizeof data),
          "control: compress succeeds, source removed, .gz inflates to the original");

    snprintf(src, sizeof src, "%s/full.log", argv[1]); snprintf(dst, sizeof dst, "%s/full.log.gz", argv[1]);
    mk(src);
    /* In a child: RLIMIT_FSIZE also limits our stdout when it is a file. */
    pid_t pid = fork();
    if (pid == 0) {
        signal(SIGXFSZ, SIG_IGN);
        struct rlimit rl = { 64, 64 };
        setrlimit(RLIMIT_FSIZE, &rl);
        _exit(compress_to_gz(src, dst) == -1 ? 0 : 1);
    }
    int st = 0;
    waitpid(pid, &st, 0);
    CHECK(WIFEXITED(st) && WEXITSTATUS(st) == 0, "gzclose() failing (EFBIG) is reported as failure");
    CHECK(same(src), "the rotated log is still there, byte-for-byte");
    CHECK(access(dst, F_OK) != 0, "the truncated .gz is removed");
    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
C

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -Wno-unused-function \
        -o "${WORK}/t" "${WORK}/t.c" -lz >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 "${WORK}/cc.log")"
    exit 1
fi
mkdir -p "${WORK}/d"
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" "${WORK}/d"
rc=$?
echo ""
[ "${rc}" -eq 0 ] && echo "C205: PASS" || echo "C205: FAIL (exit ${rc})"
exit "${rc}"
