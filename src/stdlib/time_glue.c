/*
 * time_glue.c — i64-ABI wrappers for std.time module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.time.
 */

#include "tk_time.h"
#include <stdint.h>
#include <stdlib.h>

int64_t tk_time_now_w(void) {
    return (int64_t)tk_time_now();
}
int64_t tk_time_format_w(int64_t ts, int64_t fmt) {
    const char *result = tk_time_format(
        (uint64_t)ts,
        (const char *)(intptr_t)fmt);
    return (int64_t)(intptr_t)result;
}
int64_t tk_time_parse_w(int64_t s, int64_t fmt) { (void)s; (void)fmt; return 0; }
int64_t tk_time_elapsed_w(int64_t start) { (void)start; return 0; }
int64_t tk_time_sleep_w(int64_t ms) { (void)ms; return 0; }

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
