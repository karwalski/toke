/*
 * time_glue.c — i64-ABI wrappers for std.time module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.time.
 */

#include "tk_time.h"
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
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
    int64_t h = tk_arr_alloc(6, 6);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    block[0] = (int64_t)parts.year;
    block[1] = (int64_t)parts.month;
    block[2] = (int64_t)parts.day;
    block[3] = (int64_t)parts.hour;
    block[4] = (int64_t)parts.min;
    block[5] = (int64_t)parts.sec;
    return h;
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

/* ── .tki declared wrappers ─────────────────────────────────────────────── */

/* f64<->i64 bitcast helpers */
static double i64_to_f64(int64_t i) { double d; memcpy(&d, &i, sizeof(d)); return d; }
static int64_t f64_to_i64(double d) { int64_t i; memcpy(&i, &d, sizeof(i)); return i; }

/* time.since(ts) — ms elapsed since ts */
int64_t tk_time_since_w(int64_t ts) {
    return (int64_t)tk_time_since((uint64_t)ts);
}

/* time.add(ts, dur) — add duration ms to timestamp */
int64_t tk_time_add_w(int64_t ts, int64_t dur) {
    return (int64_t)tk_time_add((uint64_t)ts, dur);
}

/* time.diff(ts1, ts2) — ts1 - ts2 in ms */
int64_t tk_time_diff_w(int64_t ts1, int64_t ts2) {
    return tk_time_diff((uint64_t)ts1, (uint64_t)ts2);
}

/* time.to_parts(ts) — returns toke array [year, month, day, hour, min, sec] */
int64_t tk_time_to_parts_w(int64_t ts) {
    return tk_time_toparts_w(ts);
}

/* time.from_parts(parts_arr) — array of [year,month,day,hour,min,sec] -> timestamp */
int64_t tk_time_from_parts_w(int64_t parts_arr) {
    if (!parts_arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)parts_arr;
    TkTimeParts p;
    p.year  = (int)ptr[0];
    p.month = (int)ptr[1];
    p.day   = (int)ptr[2];
    p.hour  = (int)ptr[3];
    p.min   = (int)ptr[4];
    p.sec   = (int)ptr[5];
    return (int64_t)tk_time_from_parts(p);
}

/* time.is_leap_year(year) */
int64_t tk_time_is_leap_year_w(int64_t year) {
    return (int64_t)tk_time_is_leap_year((int)year);
}

/* time.days_in_month(year, month) */
int64_t tk_time_days_in_month_w(int64_t year, int64_t month) {
    return (int64_t)tk_time_days_in_month((int)year, (int)month);
}

/* time.with_tz(ts, tz_name) */
int64_t tk_time_with_tz_w(int64_t ts, int64_t tz) {
    const char *tz_name = tz ? (const char *)(intptr_t)tz : "UTC";
    const char *result = tk_time_with_tz((uint64_t)ts, tz_name);
    return result ? (int64_t)(intptr_t)result : 0;
}

/* time.utc_offset(tz_name) */
int64_t tk_time_utc_offset_w(int64_t tz) {
    const char *tz_name = tz ? (const char *)(intptr_t)tz : "UTC";
    return tk_time_utc_offset(tz_name);
}

/* time.convert(ts, from_tz, to_tz) */
int64_t tk_time_convert_w(int64_t ts, int64_t from_tz, int64_t to_tz) {
    const char *from = from_tz ? (const char *)(intptr_t)from_tz : "UTC";
    const char *to = to_tz ? (const char *)(intptr_t)to_tz : "UTC";
    const char *result = tk_time_convert((uint64_t)ts, from, to);
    return result ? (int64_t)(intptr_t)result : 0;
}

/* time.add_days(ts, n) */
int64_t tk_time_add_days_w(int64_t ts, int64_t n) {
    return (int64_t)tk_time_add_days((uint64_t)ts, n);
}

/* time.add_months(ts, n) */
int64_t tk_time_add_months_w(int64_t ts, int64_t n) {
    return (int64_t)tk_time_add_months((uint64_t)ts, n);
}

/* time.add_years(ts, n) */
int64_t tk_time_add_years_w(int64_t ts, int64_t n) {
    return (int64_t)tk_time_add_years((uint64_t)ts, n);
}

/* time.start_of_day(ts) */
int64_t tk_time_start_of_day_w(int64_t ts) {
    return (int64_t)tk_time_start_of_day((uint64_t)ts);
}

/* time.start_of_month(ts) */
int64_t tk_time_start_of_month_w(int64_t ts) {
    return (int64_t)tk_time_start_of_month((uint64_t)ts);
}

/* time.start_of_year(ts) */
int64_t tk_time_start_of_year_w(int64_t ts) {
    return (int64_t)tk_time_start_of_year((uint64_t)ts);
}

/* time.parse_duration(s) — returns toke array [years,months,days,hours,minutes,seconds] */
int64_t tk_time_parse_duration_w(int64_t s) {
    if (!s) return 0;
    TkDurationParseResult r = tk_time_parse_duration((const char *)(intptr_t)s);
    if (r.is_err) return 0;
    int64_t h = tk_arr_alloc(6, 6);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    block[0] = (int64_t)r.ok.years;
    block[1] = (int64_t)r.ok.months;
    block[2] = (int64_t)r.ok.days;
    block[3] = (int64_t)r.ok.hours;
    block[4] = (int64_t)r.ok.minutes;
    block[5] = (int64_t)r.ok.seconds;
    return h;
}

/* time.format_duration(dur_arr) — array of [y,mo,d,h,m,s] -> string */
int64_t tk_time_format_duration_w(int64_t dur_arr) {
    if (!dur_arr) return (int64_t)(intptr_t)"0s";
    int64_t *ptr = (int64_t *)(intptr_t)dur_arr;
    TkDuration d;
    d.years   = (int)ptr[0];
    d.months  = (int)ptr[1];
    d.days    = (int)ptr[2];
    d.hours   = (int)ptr[3];
    d.minutes = (int)ptr[4];
    d.seconds = (int)ptr[5];
    const char *result = tk_time_format_duration(d);
    return result ? (int64_t)(intptr_t)result : (int64_t)(intptr_t)"0s";
}

/* time.duration(from, to) — structured duration between timestamps */
int64_t tk_time_duration_w(int64_t from, int64_t to) {
    TkDuration d = tk_time_duration((uint64_t)from, (uint64_t)to);
    int64_t h = tk_arr_alloc(6, 6);
    if (!h) return 0;
    int64_t *block = (int64_t *)(intptr_t)h;
    block[0] = (int64_t)d.years;
    block[1] = (int64_t)d.months;
    block[2] = (int64_t)d.days;
    block[3] = (int64_t)d.hours;
    block[4] = (int64_t)d.minutes;
    block[5] = (int64_t)d.seconds;
    return h;
}

/* time.julian_date(ts) — returns f64 as i64 bits */
int64_t tk_time_julian_date_w(int64_t ts) {
    return f64_to_i64(tk_time_julian_date((uint64_t)ts));
}

/* time.mars_sol(ts) — returns f64 as i64 bits */
int64_t tk_time_mars_sol_w(int64_t ts) {
    return f64_to_i64(tk_time_mars_sol((uint64_t)ts));
}

/* time.format_mars(sol_bits, fmt) */
int64_t tk_time_format_mars_w(int64_t sol_bits, int64_t fmt) {
    double sol = i64_to_f64(sol_bits);
    const char *fstr = fmt ? (const char *)(intptr_t)fmt : "Sol %d %H:%M:%S MTC";
    const char *result = tk_time_format_mars(sol, fstr);
    return result ? (int64_t)(intptr_t)result : 0;
}

/* time.light_delay(from_body, to_body, ts) — returns f64 as i64 bits */
int64_t tk_time_light_delay_w(int64_t from, int64_t to, int64_t ts) {
    const char *fb = from ? (const char *)(intptr_t)from : "earth";
    const char *tb = to ? (const char *)(intptr_t)to : "mars";
    return f64_to_i64(tk_time_light_delay(fb, tb, (uint64_t)ts));
}
