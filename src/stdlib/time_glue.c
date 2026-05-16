/*
 * time_glue.c — i64-ABI wrappers for std.time module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.time.
 */

#include "tk_time.h"
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

int64_t tk_time_now_w(void) {
    return (int64_t)tk_time_now();
}
int64_t tk_time_format_w(int64_t ts, int64_t fmt) {
    const char *result = tk_time_format(
        (uint64_t)ts,
        (const char *)(intptr_t)fmt);
    return (int64_t)(intptr_t)result;
}
int64_t tk_time_parse_w(int64_t s, int64_t fmt) {
    if (!s || !fmt) return 0;
    const char *sstr = (const char *)(intptr_t)s;
    const char *fstr = (const char *)(intptr_t)fmt;
    TimeParseResult r = tk_time_parse(sstr, fstr);
    if (r.is_err) return 0;
    return (int64_t)r.ok;
}
int64_t tk_time_elapsed_w(int64_t start) {
    return (int64_t)tk_time_since((uint64_t)start);
}
int64_t tk_time_sleep_w(int64_t ms) {
    if (ms <= 0) return 0;
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&req, NULL);
    return 0;
}

int64_t tk_time_toparts_w(int64_t ts) {
    TkTimeParts parts = tk_time_to_parts((uint64_t)ts);
    int64_t *block = (int64_t *)malloc(7 * sizeof(int64_t));
    if (!block) return 0;
    block[0] = 6;
    block[1] = (int64_t)parts.year;
    block[2] = (int64_t)parts.month;
    block[3] = (int64_t)parts.day;
    block[4] = (int64_t)parts.hour;
    block[5] = (int64_t)parts.min;
    block[6] = (int64_t)parts.sec;
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_time_weekday_w(int64_t ts) {
    return (int64_t)tk_time_weekday((uint64_t)ts);
}

/* ── Linker-gap additions ───────────────────────────────────────────────── */

/* time.nowms() — current Unix timestamp in milliseconds (same as time.now) */
int64_t tk_time_nowms_w(int64_t dummy) {
    (void)dummy;
    return (int64_t)tk_time_now();
}

/* time.nowunix() — current Unix timestamp in seconds */
int64_t tk_time_nowunix_w(int64_t dummy) {
    (void)dummy;
    return (int64_t)(tk_time_now() / 1000);
}

/* time.unixnow() — alias for nowunix */
int64_t tk_time_unixnow_w(int64_t dummy) {
    return tk_time_nowunix_w(dummy);
}

/* time.elapsedms(start) — milliseconds elapsed since start timestamp */
int64_t tk_time_elapsedms_w(int64_t start) {
    return (int64_t)tk_time_since((uint64_t)start);
}

/* time.formatnow(fmt) — format current time with given strftime format */
int64_t tk_time_formatnow_w(int64_t fmt) {
    if (!fmt) fmt = (int64_t)(intptr_t)"%Y-%m-%dT%H:%M:%SZ";
    uint64_t now = tk_time_now();
    const char *result = tk_time_format(now, (const char *)(intptr_t)fmt);
    return (int64_t)(intptr_t)result;
}

/* time.sleepms(ms) — sleep for given milliseconds */
int64_t tk_time_sleepms_w(int64_t ms) {
    if (ms <= 0) return 0;
    struct timespec req;
    req.tv_sec  = (time_t)(ms / 1000);
    req.tv_nsec = (long)((ms % 1000) * 1000000L);
    nanosleep(&req, NULL);
    return 0;
}
