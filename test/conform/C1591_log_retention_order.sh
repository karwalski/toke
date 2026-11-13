#!/usr/bin/env bash
# C1591_log_retention_order.sh -- access-log retention keeps the NEWEST
# rotated archives, deterministically, even when they share an mtime second
# (story 127.401).
#
# apply_retention used to sort rotated files by st_mtime (whole seconds)
# newest first and delete past max_files.  Rotations inside one second tied,
# qsort's order among ties is unspecified, and the deleted file could be the
# newest.  The order is now total: mtime seconds, then nanoseconds, then the
# rotation stamp in the name (stem.YYYYMMDD-HHMMSS.uuuuuu-PID[-N]), then the
# probe sequence N numerically, then the name.
#
# No sleeps: every tie is built with utimes() on archives the driver creates,
# then one real rotation (max_lines=1) triggers retention.  File count is
# capped at 8 per case and every file is a few bytes.
#
# WHAT IS ASSERTED (C, ASan+UBSan, TK_STR_VERIFY=1):
#   1. five writes at max_lines=1, max_files=2 keep exactly the archives
#      holding /line3 and /line4 (the 127.401 report: /line0 and /line3 kept).
#   2. whole-second ties (all at the same mtime, nsec 0) + one real rotation
#      at max_files=3: kept = the real one, ...000002-5-10 and ...000002-5-2
#      (N compares numerically: "-10" > "-2" although it sorts first as
#      text); deleted = ...000002-5-1, ...000002-5, ...000001-5 and an
#      unstamped rot.zzz.log.gz.  Run 3 times over fresh directories.
#   3. sub-second mtimes in one second (100/200/300 us) whose name stamps run
#      the OTHER way + one real rotation at max_files=2: the mtime wins --
#      kept = the real one and the 300 us file.  CONFORM-SKIP when the file
#      system does not keep sub-second mtimes (checked with stat).
#
# Negative control: C1591_STDLIB=<dir> uses another tree's src/stdlib.  The
# base's (refactor/z180-str-s1-tranche11, 76574959) whole-second sort fails
# cases 2 (all three runs, kept seq 0 and the unstamped file) and 3, and
# case 1 when its rotations share a second: measured 1 passed 5 failed, then
# 2 passed 4 failed.  This branch: 6 passed.
#
# Story: 127.401
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1591_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1591_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1591: access-log retention order is newest-first and deterministic (127.401)"
echo "--------------------------------------------------------------------------------"

cat > drv.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <zlib.h>
#include "log.h"

static char base[900];

static void mk(const char *dir, const char *name, long sec, long usec)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    if (!f) { printf("ERROR create %s\n", p); exit(3); }
    fputs("x\n", f);
    fclose(f);
    struct timeval tv[2] = { { sec, (int)usec }, { sec, (int)usec } };
    if (utimes(p, tv) != 0) { printf("ERROR utimes %s\n", p); exit(3); }
}

/* Print the .gz names in dir, one per line, prefixed by tag; a real (driver
 * rotated) archive prints as REAL plus the access line it holds. */
static void list(const char *tag, const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { printf("ERROR opendir %s\n", dir); exit(3); }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        size_t n = strlen(e->d_name);
        if (n < 3 || strcmp(e->d_name + n - 3, ".gz") != 0) continue;
        char p[1100];
        snprintf(p, sizeof p, "%s/%s", dir, e->d_name);
        gzFile g = gzopen(p, "rb");
        char buf[512] = "";
        if (g) { if (!gzgets(g, buf, sizeof buf)) buf[0] = '\0'; gzclose(g); }
        const char *q = strstr(buf, "\"GET ");
        if (q) {
            char line[64] = "";
            sscanf(q, "\"GET %63s", line);
            printf("%s REAL %s\n", tag, line);
        } else {
            printf("%s %s\n", tag, e->d_name);
        }
    }
    closedir(d);
}

static void rotate_once(const char *dir, int max_files, const char *line)
{
    char p[1100];
    snprintf(p, sizeof p, "%s/rot.log", dir);
    TkAccessLog *a = tk_access_log_open(p, 1, max_files, 0);
    if (!a) { printf("ERROR open %s\n", p); exit(4); }
    tk_access_log_write(a, "1.2.3.4", "GET", line, 200, 1, NULL, NULL);
    tk_access_log_close(a);
}

static void sub(char *out, size_t n, const char *name)
{
    snprintf(out, n, "%s/%s", base, name);
    mkdir(out, 0700);
}

int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    snprintf(base, sizeof base, "%s", argv[1]);
    char d[1000];

    /* 1. five real writes, keep 2 */
    sub(d, sizeof d, "five");
    {
        char p[1100];
        snprintf(p, sizeof p, "%s/rot.log", d);
        TkAccessLog *a = tk_access_log_open(p, 1, 2, 0);
        if (!a) { printf("ERROR open five\n"); return 4; }
        for (int i = 0; i < 5; i++) {
            char l[32]; snprintf(l, sizeof l, "/line%d", i);
            tk_access_log_write(a, "1.2.3.4", "GET", l, 200, 1, NULL, NULL);
        }
        tk_access_log_close(a);
    }
    list("five", d);

    /* 2. whole-second ties, three fresh runs */
    for (int r = 0; r < 3; r++) {
        char name[32]; snprintf(name, sizeof name, "ties%d", r);
        sub(d, sizeof d, name);
        const char *names[] = {
            "rot.20260101-000000.000001-5.log.gz",
            "rot.20260101-000000.000002-5.log.gz",
            "rot.20260101-000000.000002-5-1.log.gz",
            "rot.20260101-000000.000002-5-2.log.gz",
            "rot.20260101-000000.000002-5-10.log.gz",
            "rot.zzz.log.gz",
        };
        for (size_t i = 0; i < sizeof names / sizeof *names; i++)
            mk(d, names[i], 1000000000L, 0);
        rotate_once(d, 3, "/tie");
        list(name, d);
    }

    /* 3. sub-second mtimes against reversed name stamps */
    sub(d, sizeof d, "nsec");
    mk(d, "rot.20260101-000000.000003-5.log.gz", 1000000000L, 100);
    mk(d, "rot.20260101-000000.000002-5.log.gz", 1000000000L, 200);
    mk(d, "rot.20260101-000000.000001-5.log.gz", 1000000000L, 300);
    {
        char p[1100]; struct stat st;
        snprintf(p, sizeof p, "%s/rot.20260101-000000.000001-5.log.gz", d);
        long ns = -1;
        if (stat(p, &st) == 0) {
#if defined(__APPLE__)
            ns = (long)st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
            ns = (long)st.st_mtim.tv_nsec;
#endif
        }
        printf("nsec-kept %ld\n", ns);
    }
    rotate_once(d, 2, "/sub");
    list("nsec", d);
    printf("done\n");
    return 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects log -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -n "${LINKSET}" ] && "${CC}" "${CF[@]}" -o drv drv.c "${LS[@]}" -lz -lm -lpthread > drv.cc 2>&1; then
    mkdir -p out
    TK_STR_VERIFY=1 ./drv "${WORK}/out" > d.out 2>&1; rc=$?
    if [ "${rc}" -ne 0 ] || ! grep -q '^done$' d.out; then
        fail "driver died (rc=${rc}): $(grep -a -m2 -E 'ERROR|runtime error|abort' d.out)"
    fi
    python3 - d.out > py.out <<'PY'
import sys
L = open(sys.argv[1], encoding="utf-8", errors="replace").read().split("\n")
def kept(tag): return sorted(l[len(tag) + 1:] for l in L if l.startswith(tag + " "))
def check(ok, msg, got): print(("PASS " if ok else "FAIL ") + msg + ("" if ok else ": got %r" % (got,)))
k = kept("five")
check(k == ["REAL /line3", "REAL /line4"], "five writes, keep 2: the archives of /line3 and /line4 remain", k)
want = sorted(["REAL /tie", "rot.20260101-000000.000002-5-10.log.gz", "rot.20260101-000000.000002-5-2.log.gz"])
for r in range(3):
    k = kept("ties%d" % r)
    check(k == want, "whole-second ties run %d: real + stamp .000002 seq 10 and 2 kept (seq numeric)" % r, k)
ns = [l for l in L if l.startswith("nsec-kept ")]
if ns and ns[0].split()[1] == "300000":
    k = kept("nsec")
    check(k == ["REAL /sub", "rot.20260101-000000.000001-5.log.gz"],
          "sub-second mtime beats the name stamp: real + the 300us file kept", k)
else:
    print("SKIP sub-second mtimes not kept by this file system (%s)" % (ns[0] if ns else "no stat"))
PY
    while IFS= read -r line; do
        case "${line}" in
            PASS\ *) pass "${line#PASS }" ;;
            FAIL\ *) fail "${line#FAIL }" ;;
            SKIP\ *) echo "  CONFORM-SKIP: ${line#SKIP }" ;;
        esac
    done < py.out
    if [ "${rc}" -eq 0 ] && grep -q '^done$' d.out; then pass "driver ran clean under ASan+UBSan and TK_STR_VERIFY=1"; fi
else
    fail "C driver did not build"; sed 's/^/        /' drv.cc 2>/dev/null | head -6
fi

echo "--------------------------------------------------------------------------------"
echo "C1591: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
