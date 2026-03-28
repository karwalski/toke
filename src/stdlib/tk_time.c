/*
 * tk_time.c — Implementation of the std.time standard library module.
 *
 * Named tk_time.c (not time.c) to avoid shadowing the POSIX <time.h> header.
 *
 * Uses clock_gettime(CLOCK_REALTIME) for now() and since(),
 * and strftime for format().
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 2.7.4  Branch: feature/stdlib-2.7-crypto-time-test
 */

#include "tk_time.h"
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

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
