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

/* Story: 57.11.2 — timezone-aware time operations */

/* time.with_tz(ts:u64;tz_name:Str):Str
 * Formats a timestamp in the given IANA timezone (e.g. "America/New_York").
 * Returns ISO 8601 string in that timezone. Caller owns the returned string. */
const char *tk_time_with_tz(uint64_t ts_ms, const char *tz_name);

/* time.utc_offset(tz_name:Str):i64
 * Returns the current UTC offset in seconds for the given IANA timezone.
 * Returns 0 if the timezone is unknown. */
int64_t tk_time_utc_offset(const char *tz_name);

/* time.convert(ts:u64;from_tz:Str;to_tz:Str):Str
 * Converts a display time from one timezone to another.
 * Returns ISO 8601 string in the target timezone. */
const char *tk_time_convert(uint64_t ts_ms, const char *from_tz, const char *to_tz);

/* Story: 57.11.3 — calendar arithmetic */

/* time.add_days(ts:u64;n:i64):u64 — add n days to timestamp */
uint64_t tk_time_add_days(uint64_t ts_ms, int64_t n);

/* time.add_months(ts:u64;n:i64):u64 — add n months with month-end clamping */
uint64_t tk_time_add_months(uint64_t ts_ms, int64_t n);

/* time.add_years(ts:u64;n:i64):u64 — add n years with leap year handling */
uint64_t tk_time_add_years(uint64_t ts_ms, int64_t n);

/* time.start_of_day(ts:u64):u64 — truncate to midnight UTC */
uint64_t tk_time_start_of_day(uint64_t ts_ms);

/* time.start_of_month(ts:u64):u64 — first day of month, midnight UTC */
uint64_t tk_time_start_of_month(uint64_t ts_ms);

/* time.start_of_year(ts:u64):u64 — January 1, midnight UTC */
uint64_t tk_time_start_of_year(uint64_t ts_ms);

/* Story: 57.11.4 — duration parsing and formatting */

typedef struct {
    int years;
    int months;
    int days;
    int hours;
    int minutes;
    int seconds;
} TkDuration;

/* Parse ISO 8601 duration string (e.g. "P1Y2M3DT4H5M6S"). */
typedef struct {
    TkDuration ok;
    int        is_err;
    const char *err_msg;
} TkDurationParseResult;

TkDurationParseResult tk_time_parse_duration(const char *s);

/* Format a duration as human-readable string (e.g. "1y 2mo 3d 4h 5m 6s"). */
const char *tk_time_format_duration(TkDuration d);

/* Compute structured duration between two timestamps. */
TkDuration tk_time_duration(uint64_t from_ms, uint64_t to_ms);

/* Story: 57.11.5-57.11.7 — interplanetary time */

/* time.julian_date(ts:u64):f64
 * Convert Unix timestamp to Julian Date. */
double tk_time_julian_date(uint64_t ts_ms);

/* time.mars_sol(ts:u64):f64
 * Convert Unix timestamp to Mars Sol Date (MSD). */
double tk_time_mars_sol(uint64_t ts_ms);

/* time.format_mars(sol:f64;fmt:Str):Str
 * Format a Mars sol as a string (e.g. "Sol 3042 14:23:07 MTC"). */
const char *tk_time_format_mars(double sol, const char *fmt);

/* time.light_delay(from_body:Str;to_body:Str;ts:u64):f64
 * Approximate one-way light-time delay in seconds between two bodies.
 * Uses mean orbital distances. Bodies: "earth", "mars", "moon", "sun". */
double tk_time_light_delay(const char *from_body, const char *to_body,
                           uint64_t ts_ms);

#endif /* TK_STDLIB_TK_TIME_H */
