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

    /* --- tk_time_parse() --- */

    /* Parse a known date with %Y-%m-%d; result must be non-zero */
    TimeParseResult pr1 = tk_time_parse("2024-01-15", "%Y-%m-%d");
    ASSERT(pr1.is_err == 0, "parse('2024-01-15','%Y-%m-%d') succeeds");
    ASSERT(pr1.ok > 0, "parse('2024-01-15') returns non-zero timestamp");

    /* The known timestamp for 2024-01-15 00:00:00 UTC = 1705276800 seconds */
    ASSERT(pr1.ok == 1705276800ULL * 1000ULL,
           "parse('2024-01-15') matches expected UTC ms");

    /* Empty string must produce is_err=1 */
    TimeParseResult pr2 = tk_time_parse("", "%Y-%m-%d");
    ASSERT(pr2.is_err == 1, "parse('') gives is_err=1");
    ASSERT(pr2.err_msg != NULL, "parse('') err_msg is not NULL");

    /* NULL string must produce is_err=1 */
    TimeParseResult pr3 = tk_time_parse(NULL, "%Y-%m-%d");
    ASSERT(pr3.is_err == 1, "parse(NULL) gives is_err=1");

    /* String that does not match format */
    TimeParseResult pr4 = tk_time_parse("not-a-date", "%Y-%m-%d");
    ASSERT(pr4.is_err == 1, "parse('not-a-date') gives is_err=1");

    /* Parse with time format */
    TimeParseResult pr5 = tk_time_parse("2024-01-15T12:34:56", "%Y-%m-%dT%H:%M:%S");
    ASSERT(pr5.is_err == 0, "parse ISO datetime succeeds");
    ASSERT(pr5.ok > 0, "parse ISO datetime returns non-zero timestamp");

    /* --- tk_time_add() --- */

    /* Add 1 hour (3 600 000 ms) to a base timestamp */
    uint64_t base_ms = 1705276800ULL * 1000ULL; /* 2024-01-15 00:00:00 UTC */
    uint64_t added   = tk_time_add(base_ms, 3600000LL);
    ASSERT(added == base_ms + 3600000ULL, "add(base, 1h) == base + 3600000");

    /* Subtract 1 hour via negative duration */
    uint64_t subtracted = tk_time_add(base_ms, -3600000LL);
    ASSERT(subtracted == base_ms - 3600000ULL, "add(base, -1h) == base - 3600000");

    /* Saturate on underflow: subtracting more than ts_ms holds */
    uint64_t clamped_low = tk_time_add(500ULL, -1000LL);
    ASSERT(clamped_low == 0, "add underflow saturates to 0");

    /* Saturate on overflow */
    uint64_t clamped_high = tk_time_add(UINT64_MAX, 1LL);
    ASSERT(clamped_high == UINT64_MAX, "add overflow saturates to UINT64_MAX");

    /* Zero duration is a no-op */
    ASSERT(tk_time_add(base_ms, 0LL) == base_ms, "add(base, 0) == base");

    /* --- tk_time_diff() --- */

    /* Two timestamps 1 second apart → diff = 1000 ms */
    uint64_t ts_a = 1705276800ULL * 1000ULL;
    uint64_t ts_b = ts_a + 1000ULL;
    int64_t  diff1 = tk_time_diff(ts_b, ts_a);
    ASSERT(diff1 == 1000LL, "diff(ts+1s, ts) == 1000 ms");

    /* Reversed order → negative */
    int64_t diff2 = tk_time_diff(ts_a, ts_b);
    ASSERT(diff2 == -1000LL, "diff(ts, ts+1s) == -1000 ms");

    /* Same timestamp → zero */
    ASSERT(tk_time_diff(ts_a, ts_a) == 0LL, "diff(ts, ts) == 0");

    /* --- Story 28.3.2: tk_time_to_parts() --- */

    /* Unix epoch (ts=0) must break down to 1970-01-01 00:00:00 UTC */
    TkTimeParts epoch = tk_time_to_parts(0ULL);
    ASSERT(epoch.year  == 1970, "to_parts(0).year == 1970");
    ASSERT(epoch.month == 1,    "to_parts(0).month == 1");
    ASSERT(epoch.day   == 1,    "to_parts(0).day == 1");
    ASSERT(epoch.hour  == 0,    "to_parts(0).hour == 0");
    ASSERT(epoch.min   == 0,    "to_parts(0).min == 0");
    ASSERT(epoch.sec   == 0,    "to_parts(0).sec == 0");

    /* --- Story 28.3.2: tk_time_from_parts() round-trip --- */

    TkTimeParts input;
    input.year  = 2024;
    input.month = 1;
    input.day   = 15;
    input.hour  = 12;
    input.min   = 0;
    input.sec   = 0;
    uint64_t rt_ts = tk_time_from_parts(input);
    ASSERT(rt_ts > 0, "from_parts({2024,1,15,12,0,0}) returns non-zero timestamp");
    TkTimeParts rt = tk_time_to_parts(rt_ts);
    ASSERT(rt.year  == 2024, "from_parts round-trip: year == 2024");
    ASSERT(rt.month == 1,    "from_parts round-trip: month == 1");
    ASSERT(rt.day   == 15,   "from_parts round-trip: day == 15");
    ASSERT(rt.hour  == 12,   "from_parts round-trip: hour == 12");
    ASSERT(rt.min   == 0,    "from_parts round-trip: min == 0");
    ASSERT(rt.sec   == 0,    "from_parts round-trip: sec == 0");

    /* --- Story 28.3.2: tk_time_weekday() --- */

    /* 2024-01-15 was a Monday (weekday == 1).
     * Use the same fixed_ms as format() tests: 1705322096000 ms (12:34:56 UTC).
     * The date portion is still 2024-01-15, so weekday must be 1. */
    uint8_t wd = tk_time_weekday(fixed_ms);
    ASSERT(wd == 1, "weekday(2024-01-15) == 1 (Monday)");

    /* --- Story 28.3.2: tk_time_is_leap_year() --- */

    ASSERT(tk_time_is_leap_year(2024) == 1, "is_leap_year(2024) == 1");
    ASSERT(tk_time_is_leap_year(2023) == 0, "is_leap_year(2023) == 0");
    ASSERT(tk_time_is_leap_year(1900) == 0, "is_leap_year(1900) == 0 (century, not /400)");
    ASSERT(tk_time_is_leap_year(2000) == 1, "is_leap_year(2000) == 1 (divisible by 400)");

    /* --- Story 28.3.2: tk_time_days_in_month() --- */

    ASSERT(tk_time_days_in_month(2024, 2) == 29, "days_in_month(2024, 2) == 29 (leap Feb)");
    ASSERT(tk_time_days_in_month(2023, 2) == 28, "days_in_month(2023, 2) == 28 (non-leap Feb)");
    ASSERT(tk_time_days_in_month(2024, 1) == 31, "days_in_month(2024, 1) == 31 (January)");

    if (failures == 0) { printf("All time tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
