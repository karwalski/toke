#ifndef TK_STDLIB_TK_TIME_H
#define TK_STDLIB_TK_TIME_H

/*
 * tk_time.h — C interface for the std.time standard library module.
 *
 * Named tk_time.h (not time.h) to avoid shadowing the POSIX <time.h> header.
 *
 * Type mappings:
 *   u64  = uint64_t
 *   Str  = const char *  (null-terminated UTF-8)
 *
 * Story: 2.7.4  Branch: feature/stdlib-2.7-crypto-time-test
 * Story: 28.3.1
 */

#include <stdint.h>

/* time.now():u64
 * Returns the current Unix timestamp in milliseconds. */
uint64_t tk_time_now(void);

/* time.format(ts:u64;fmt:Str):Str
 * Formats a millisecond Unix timestamp using strftime format codes.
 * Returns a heap-allocated string; caller owns it. */
const char *tk_time_format(uint64_t ts_ms, const char *fmt);

/* time.since(ts:u64):u64
 * Returns the number of milliseconds elapsed since the given timestamp. */
uint64_t tk_time_since(uint64_t ts_ms);

/* Result type for time.parse(). */
typedef struct {
    uint64_t    ok;       /* Unix timestamp in milliseconds (valid when is_err==0) */
    int         is_err;   /* 1 if parsing failed, 0 on success */
    const char *err_msg;  /* static error string (valid when is_err==1) */
} TimeParseResult;

/* time.parse(s:Str;fmt:Str):TimeParseResult
 * Parses a time string using strptime() and returns the Unix timestamp in
 * milliseconds.  The seconds component is used; sub-second precision is
 * not available through strptime.
 * Returns is_err=1 with err_msg set if parsing fails. */
TimeParseResult tk_time_parse(const char *s, const char *fmt);

/* time.add(ts:u64;dur:i64):u64
 * Adds duration_ms (may be negative) to ts_ms.
 * Uses saturating arithmetic: clamps to 0 on underflow, UINT64_MAX on
 * overflow. */
uint64_t tk_time_add(uint64_t ts_ms, int64_t duration_ms);

/* time.diff(ts1:u64;ts2:u64):i64
 * Returns ts1_ms - ts2_ms in milliseconds (may be negative). */
int64_t tk_time_diff(uint64_t ts1_ms, uint64_t ts2_ms);

/* Story: 28.3.2 — date breakdown and calendar */

/* Broken-down UTC date/time fields. */
typedef struct {
    int year;   /* e.g. 2024 */
    int month;  /* 1-12 */
    int day;    /* 1-31 */
    int hour;   /* 0-23 */
    int min;    /* 0-59 */
    int sec;    /* 0-59 */
} TkTimeParts;

/* time.to_parts(ts:u64):TkTimeParts
 * Converts a millisecond Unix timestamp to UTC date/time fields. */
TkTimeParts tk_time_to_parts(uint64_t ts_ms);

/* time.from_parts(parts:TkTimeParts):u64
 * Converts UTC date/time fields to a millisecond Unix timestamp. */
uint64_t tk_time_from_parts(TkTimeParts parts);

/* time.weekday(ts:u64):u8
 * Returns the day of the week for the given timestamp: 0=Sun, 1=Mon, ..., 6=Sat. */
uint8_t tk_time_weekday(uint64_t ts_ms);

/* time.is_leap_year(year:int):int
 * Returns 1 if the given year is a leap year, 0 otherwise. */
int tk_time_is_leap_year(int year);

/* time.days_in_month(year:int;month:int):u8
 * Returns the number of days in the given month (1-12) of the given year. */
uint8_t tk_time_days_in_month(int year, int month);

#endif /* TK_STDLIB_TK_TIME_H */
