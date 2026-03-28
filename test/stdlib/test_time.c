/*
 * test_time.c — Unit tests for the std.time C library (Story 2.7.4).
 *
 * Build and run: make test-stdlib-time
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/tk_time.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* --- now() returns a plausible Unix ms timestamp ---
     * 1_000_000_000_000 ms = 2001-09-09 (well past epoch zero).
     * 4_000_000_000_000 ms = ~2096 (reasonable upper bound for a test). */
    uint64_t t1 = tk_time_now();
    ASSERT(t1 > 1000000000000ULL, "now() > year-2001 ms threshold");
    ASSERT(t1 < 4000000000000ULL, "now() < year-2096 ms threshold");

    /* --- now() is monotonically non-decreasing across two calls --- */
    uint64_t t2 = tk_time_now();
    ASSERT(t2 >= t1, "second now() >= first now()");

    /* --- since() is non-negative --- */
    uint64_t elapsed = tk_time_since(t1);
    ASSERT(elapsed < 5000ULL, "since(t1) < 5000 ms (test runs fast)");

    /* since() with a past timestamp returns a positive value */
    uint64_t old_ts = t1 - 10000ULL; /* 10 seconds in the past */
    uint64_t since_old = tk_time_since(old_ts);
    ASSERT(since_old >= 10000ULL, "since(t1-10s) >= 10000 ms");

    /* since() with a future timestamp returns 0 (clamped) */
    uint64_t future = t1 + 1000000ULL;
    ASSERT(tk_time_since(future) == 0, "since(future) == 0");

    /* --- format() returns a non-empty string --- */
    /* Use a fixed timestamp: 2024-01-15 12:34:56 UTC = 1705322096 seconds */
    uint64_t fixed_ms = 1705322096ULL * 1000ULL;
    const char *s1 = tk_time_format(fixed_ms, "%Y-%m-%d");
    ASSERT(s1 && strlen(s1) > 0, "format() returns non-empty string");
    ASSERT_STREQ(s1, "2024-01-15", "format(%Y-%m-%d) known date");

    const char *s2 = tk_time_format(fixed_ms, "%H:%M:%S");
    ASSERT(s2 && strlen(s2) > 0, "format(%H:%M:%S) returns non-empty");
    ASSERT_STREQ(s2, "12:34:56", "format(%H:%M:%S) known time");

    /* format() with NULL fmt falls back to ISO-8601 style (non-empty) */
    const char *s3 = tk_time_format(fixed_ms, NULL);
    ASSERT(s3 && strlen(s3) > 0, "format(NULL fmt) returns non-empty");

    /* format() with literal text */
    const char *s4 = tk_time_format(fixed_ms, "year=%Y");
    ASSERT_STREQ(s4, "year=2024", "format literal text");

    if (failures == 0) { printf("All time tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
