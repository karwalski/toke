/*
 * tk_time.c — Implementation of the std.time standard library module.
 *
 * Named tk_time.c (not time.c) to avoid shadowing the POSIX <time.h> header.
 *
 * Uses clock_gettime(CLOCK_REALTIME) for now() and since(),
 * strftime for format(), and strptime for parse().
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 2.7.4  Branch: feature/stdlib-2.7-crypto-time-test
 * Story: 28.3.1
 */

/* strptime() requires _POSIX_C_SOURCE >= 200809L.
 * Only define it if no broader feature-test macro is already active
 * (e.g. the build system may pass -D_GNU_SOURCE which is a superset). */
#if !defined(_GNU_SOURCE) && !defined(_BSD_SOURCE) && \
    !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE 200809L
#endif

#include "tk_time.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

/* timegm() is not part of C99/POSIX but is available on glibc and Apple
 * platforms.  Provide a forward declaration so the compiler sees it even
 * when _GNU_SOURCE is defined without pulling in _time.h extensions. */
#if defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
    defined(__OpenBSD__) || defined(_GNU_SOURCE)
time_t timegm(struct tm *);
#endif

/* Return current time as milliseconds since Unix epoch. */
uint64_t tk_time_now(void)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return 0;
    return (uint64_t)ts.tv_sec * 1000ULL + (uint64_t)(ts.tv_nsec / 1000000LL);
}

const char *tk_time_format(uint64_t ts_ms, const char *fmt)
{
    if (!fmt) fmt = "%Y-%m-%dT%H:%M:%S";

    time_t secs = (time_t)(ts_ms / 1000ULL);
    struct tm *tm_info = gmtime(&secs);
    if (!tm_info) return NULL;

    /* strftime output is bounded; 256 bytes covers all practical formats. */
    size_t buf_size = 256;
    char *buf = malloc(buf_size);
    if (!buf) return NULL;

    size_t written = strftime(buf, buf_size, fmt, tm_info);
    if (written == 0) {
        /* Format may need more space — try 1 KiB. */
        free(buf);
        buf_size = 1024;
        buf = malloc(buf_size);
        if (!buf) return NULL;
        written = strftime(buf, buf_size, fmt, tm_info);
        if (written == 0) {
            free(buf);
            return NULL;
        }
    }
    return buf;
}

uint64_t tk_time_since(uint64_t ts_ms)
{
    uint64_t now = tk_time_now();
    if (now <= ts_ms) return 0;
    return now - ts_ms;
}

TimeParseResult tk_time_parse(const char *s, const char *fmt)
{
    TimeParseResult result;
    result.ok      = 0;
    result.is_err  = 0;
    result.err_msg = NULL;

    if (!s || s[0] == '\0') {
        result.is_err  = 1;
        result.err_msg = "empty input string";
        return result;
    }
    if (!fmt || fmt[0] == '\0') {
        result.is_err  = 1;
        result.err_msg = "empty format string";
        return result;
    }

    struct tm tm_info;
    memset(&tm_info, 0, sizeof(tm_info));

    const char *end = strptime(s, fmt, &tm_info);
    if (end == NULL) {
        result.is_err  = 1;
        result.err_msg = "strptime failed: input does not match format";
        return result;
    }

    /* mktime interprets local time; use timegm equivalent via TZ trick or
     * manual UTC calculation.  We normalise via mktime with TZ=UTC by
     * setting tm_isdst=-1 and using timegm where available.  On platforms
     * without timegm we fall back to a portable UTC conversion. */
    tm_info.tm_isdst = -1;

#if defined(_BSD_SOURCE) || defined(_GNU_SOURCE) || \
    defined(__APPLE__) || defined(__FreeBSD__)
    time_t secs = timegm(&tm_info);
#else
    /* Portable UTC conversion: set TZ, call mktime, restore TZ. */
    const char *old_tz = getenv("TZ");
    setenv("TZ", "UTC", 1);
    tzset();
    time_t secs = mktime(&tm_info);
    if (old_tz) {
        setenv("TZ", old_tz, 1);
    } else {
        unsetenv("TZ");
    }
    tzset();
#endif

    if (secs == (time_t)-1) {
        result.is_err  = 1;
        result.err_msg = "time conversion failed";
        return result;
    }

    result.ok = (uint64_t)secs * 1000ULL;
    return result;
}

uint64_t tk_time_add(uint64_t ts_ms, int64_t duration_ms)
{
    if (duration_ms >= 0) {
        uint64_t delta = (uint64_t)duration_ms;
        /* Saturate on overflow: if ts_ms + delta would exceed UINT64_MAX */
        if (delta > UINT64_MAX - ts_ms) return UINT64_MAX;
        return ts_ms + delta;
    } else {
        /* duration_ms is negative; magnitude is safe to negate when != INT64_MIN */
        uint64_t delta;
        if (duration_ms == INT64_MIN) {
            /* INT64_MIN cannot be negated in int64_t; handle as two steps */
            delta = (uint64_t)INT64_MAX + 1ULL;
        } else {
            delta = (uint64_t)(-duration_ms);
        }
        /* Saturate on underflow */
        if (delta > ts_ms) return 0;
        return ts_ms - delta;
    }
}

int64_t tk_time_diff(uint64_t ts1_ms, uint64_t ts2_ms)
{
    if (ts1_ms >= ts2_ms) {
        uint64_t d = ts1_ms - ts2_ms;
        /* Clamp to INT64_MAX if result would overflow int64_t */
        if (d > (uint64_t)INT64_MAX) return INT64_MAX;
        return (int64_t)d;
    } else {
        uint64_t d = ts2_ms - ts1_ms;
        if (d > (uint64_t)INT64_MAX + 1ULL) return INT64_MIN;
        return -(int64_t)d;
    }
}
