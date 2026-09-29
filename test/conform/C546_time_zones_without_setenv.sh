#!/usr/bin/env bash
# C546_time_zones_without_setenv.sh — time.withtz / time.utcoffset (and
# time.convert) read the zone's rules themselves: they never write TZ, and
# they are safe to call from several threads (story 121.42 item 6).
#
# THE DEFECT, measured on main (d3a083b8):
#   tk_time_with_tz and tk_time_utc_offset did setenv("TZ", name) + tzset()
#   + localtime_r and then restored TZ (time.parse had the same trick as a
#   no-timegm fallback).  The environment is process-wide.  With six threads
#   converting while a seventh read its own local time, main's code did not
#   finish in minutes (killed; the driver now bounds it at 90 s), and its
#   single-threaded answers carried macOS strftime's %z, which disagrees with
#   the tm it formats (Asia/Kathmandu 1970: 05:30 "+0545").
#
# NOW: TZif files are read directly (RFC 8536, footer rule past the table),
# POSIX TZ strings are parsed, unknown names are UTC -- no setenv, unsetenv
# or tzset anywhere in tk_time.c.
#   1. tk_time.o references no setenv / unsetenv / putenv / tzset
#   2. withtz agrees with libc (TZ set by the DRIVER, single-threaded, %z
#      built from tm_gmtoff) for 26 zones -- file, ':'-file, POSIX strings,
#      half/quarter-hour and negative-DST zones, an unknown name -- at 160
#      instants in 1970-2038 and 2099-2100 (past every table: the footer);
#      utcoffset agrees with tm_gmtoff now.  Excluded on purpose: instants
#      before a bare POSIX rule's first transition in 1970, where libc says
#      standard time and the rule says otherwise.
#   3. six threads convert (compared to 2's answers) while this thread reads
#      its own TZ and local time 20000 times: nothing changes, nothing wrong.
#
# Negative control: C546_STDLIB=<dir> builds against another src/stdlib;
# main's fails 1, disagrees in 2, and hangs in 3 (TIMEOUT at 90 s).
#
# Story: 121.42
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C546_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c546_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C546: time zones without setenv, thread-safe (121.42)"
echo "------------------------------------------------------"
if [ ! -d /usr/share/zoneinfo ] && [ -z "${TZDIR:-}" ]; then
    echo "  SKIP: no zoneinfo database on this host"; echo "C546: 0 passed, 0 failed (skipped)"; exit 0
fi

if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -iquote "${S}" -c "${S}/tk_time.c" -o tk_time.o > cc0.log 2>&1; then
    fail "tk_time.c does not compile: $(head -c 400 cc0.log)"
else
    envw="$(nm tk_time.o | awk '$1 == "U" {print $2}' | /usr/bin/grep -a -E '^_?(setenv|unsetenv|putenv|tzset)$' | tr '\n' ' ')"
    if [ -n "${envw}" ]; then fail "1 tk_time.o still calls: ${envw}"
    else pass "1 tk_time.o references no setenv, unsetenv, putenv or tzset"; fi
fi

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include "tk_time.h"

static const char *zones[] = {
    "UTC", "America/New_York", "Europe/London", "Europe/Berlin", "Europe/Dublin",
    "Australia/Sydney", "Australia/Lord_Howe", "Asia/Kolkata", "Asia/Kathmandu",
    "Asia/Tehran", "Pacific/Chatham", "Pacific/Apia", "America/St_Johns",
    "America/Sao_Paulo", "America/Argentina/Buenos_Aires", "Africa/Casablanca",
    "Antarctica/Troll", "America/Denver", ":America/Denver", "EST5EDT",
    "<+03>-3", "UTC+3", "AEST-10AEDT,M10.1.0,M4.1.0/3", "CET-1CEST,M3.5.0,M10.5.0/3",
    "Nowhere/Zone", "Etc/GMT-14",
};
#define NZ (sizeof zones / sizeof zones[0])
#define NT 160
static int64_t ts[NT];
static char want[NZ][NT][40];

static void oracle(const char *z, int64_t t, char *out, size_t cap) {
    time_t s = (time_t)t; struct tm tm;
    localtime_r(&s, &tm);
    /* %z is built from tm_gmtoff: macOS strftime's %z does not always agree
     * with the tm it is formatting (Europe/London 1970: 01:00 "+0000"). */
    size_t w = strftime(out, cap, "%Y-%m-%dT%H:%M:%S", &tm);
    long a = tm.tm_gmtoff < 0 ? -tm.tm_gmtoff : tm.tm_gmtoff;
    snprintf(out + w, cap - w, "%c%02ld%02ld", tm.tm_gmtoff < 0 ? '-' : '+', a / 3600, (a / 60) % 60);
    (void)z;
}

static int fails = 0, checks = 0;
static void *hammer(void *arg) {
    long id = (long)arg;
    int bad = 0;
    for (int it = 0; it < 40; it++)
        for (size_t z = (size_t)id % NZ; z < NZ; z += 3)
            for (int k = (int)(it % 7); k < NT; k += 7) {
                const char *g = tk_time_with_tz((uint64_t)ts[k] * 1000ULL, zones[z]);
                if (!g || strcmp(g, want[z][k]) != 0) bad++;
                free((void *)g);
            }
    return (void *)(intptr_t)bad;
}

static void on_alarm(int sig) {
    (void)sig;
    static const char m[] = "  FAIL: concurrent conversions did not finish within 90 s (hung)\n";
    ssize_t w = write(1, m, sizeof m - 1); (void)w;
    _exit(3);
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGALRM, on_alarm);
    alarm(90);
    /* timestamps: fixed edges, then every ~13 days over 2023-2025 and 2098-2100 */
    int n = 0;
    int64_t fixed[] = { 10368000, 1000000000, 1710053999, 1710054000, 1730613599, 1730613600,
                        2147483647, 2147483648LL, 4102444800LL, 4118000000LL };
    for (size_t i = 0; i < sizeof fixed / sizeof fixed[0]; i++) ts[n++] = fixed[i];
    for (int64_t t = 1672531200; n < 90; t += 13 * 86400 + 3607) ts[n++] = t;
    for (int64_t t = 4070908800LL; n < NT; t += 13 * 86400 + 3607) ts[n++] = t;

    /* 1. against libc (the oracle sets TZ -- single-threaded, before any thread) */
    for (size_t z = 0; z < NZ; z++) {
        setenv("TZ", zones[z], 1); tzset();
        for (int k = 0; k < NT; k++) oracle(zones[z], ts[k], want[z][k], sizeof want[z][k]);
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
        long off_want = tm.tm_gmtoff;
        unsetenv("TZ"); tzset();
        int bad = 0; char first[160] = "";
        for (int k = 0; k < NT; k++) {
            const char *g = tk_time_with_tz((uint64_t)ts[k] * 1000ULL, zones[z]);
            checks++;
            if (!g || strcmp(g, want[z][k]) != 0) {
                if (!bad) snprintf(first, sizeof first, "t=%lld got %s want %s",
                                   (long long)ts[k], g ? g : "<NULL>", want[z][k]);
                bad++;
            }
            free((void *)g);
        }
        int64_t off = tk_time_utc_offset(zones[z]);
        if (off != off_want) { bad++; if (!first[0]) snprintf(first, sizeof first, "utcoffset %lld want %ld", (long long)off, off_want); }
        if (bad) { fails++; printf("  FAIL: %s: %d of %d disagree with libc, first: %s\n", zones[z], bad, NT + 1, first); }
    }
    if (!fails) printf("  PASS: withtz/utcoffset agree with libc for %d zones x %d instants\n", (int)NZ, NT);

    /* 2. concurrency: 6 threads convert while this thread reads its own zone */
    setenv("TZ", "Asia/Tokyo", 1); tzset();
    time_t probe = 1700000000; struct tm pt; char mine[40], again[40];
    localtime_r(&probe, &pt); strftime(mine, sizeof mine, "%H%z", &pt);
    pthread_t th[6];
    for (long i = 0; i < 6; i++) pthread_create(&th[i], NULL, hammer, (void *)i);
    int drift = 0;
    for (int i = 0; i < 20000; i++) {
        localtime_r(&probe, &pt); strftime(again, sizeof again, "%H%z", &pt);
        if (strcmp(again, mine) != 0) drift++;
        const char *e = getenv("TZ");
        if (!e || strcmp(e, "Asia/Tokyo") != 0) drift++;
    }
    long wrong = 0;
    for (int i = 0; i < 6; i++) { void *r; pthread_join(th[i], &r); wrong += (long)(intptr_t)r; }
    if (drift) { fails++; printf("  FAIL: this thread saw another zone %d time(s) while the others converted\n", drift); }
    else printf("  PASS: this thread's TZ and localtime never changed while 6 threads converted\n");
    if (wrong) { fails++; printf("  FAIL: %ld concurrent conversions were wrong\n", wrong); }
    else printf("  PASS: every concurrent conversion was right\n");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
# shellcheck disable=SC2086
if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 -Wno-deprecated-declarations -iquote "${S}" -o t t.c "${S}/tk_time.c" -lpthread > cc.log 2>&1; then
    fail "driver does not compile: $(head -c 400 cc.log)"
else
    ./t > t.out 2>&1; rc=$?
    /usr/bin/grep -a '^  PASS\|^  FAIL' t.out | sed -e 's/^  PASS: /  PASS: 2-3 /' -e 's/^  FAIL: /  FAIL: 2-3 /'
    PASS=$((PASS + $(/usr/bin/grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(/usr/bin/grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && [ "$(/usr/bin/grep -a -c '^  FAIL' t.out)" -eq 0 ]; then
        fail "driver exit ${rc}: $(tail -2 t.out)"
    fi
fi
echo ""
echo "C546: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
