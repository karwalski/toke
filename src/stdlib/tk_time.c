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

/* --- Story 28.3.2: date breakdown and calendar --- */

TkTimeParts tk_time_to_parts(uint64_t ts_ms)
{
    TkTimeParts parts;
    time_t secs = (time_t)(ts_ms / 1000ULL);
    struct tm tm_info;
    memset(&tm_info, 0, sizeof(tm_info));
    gmtime_r(&secs, &tm_info);
    parts.year  = tm_info.tm_year + 1900;
    parts.month = tm_info.tm_mon + 1;
    parts.day   = tm_info.tm_mday;
    parts.hour  = tm_info.tm_hour;
    parts.min   = tm_info.tm_min;
    parts.sec   = tm_info.tm_sec;
    return parts;
}

uint64_t tk_time_from_parts(TkTimeParts parts)
{
    struct tm tm_info;
    memset(&tm_info, 0, sizeof(tm_info));
    tm_info.tm_year  = parts.year - 1900;
    tm_info.tm_mon   = parts.month - 1;
    tm_info.tm_mday  = parts.day;
    tm_info.tm_hour  = parts.hour;
    tm_info.tm_min   = parts.min;
    tm_info.tm_sec   = parts.sec;
    tm_info.tm_isdst = -1;
    time_t secs = timegm(&tm_info);
    if (secs == (time_t)-1) return 0;
    return (uint64_t)secs * 1000ULL;
}

uint8_t tk_time_weekday(uint64_t ts_ms)
{
    time_t secs = (time_t)(ts_ms / 1000ULL);
    struct tm tm_info;
    memset(&tm_info, 0, sizeof(tm_info));
    gmtime_r(&secs, &tm_info);
    return (uint8_t)tm_info.tm_wday; /* 0=Sun, 1=Mon, ..., 6=Sat */
}

int tk_time_is_leap_year(int year)
{
    if (year % 400 == 0) return 1;
    if (year % 100 == 0) return 0;
    if (year % 4   == 0) return 1;
    return 0;
}

uint8_t tk_time_days_in_month(int year, int month)
{
    static const uint8_t days[13] = {
        0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    if (month < 1 || month > 12) return 0;
    if (month == 2 && tk_time_is_leap_year(year)) return 29;
    return days[month];
}

/* --- Story 57.11.2: timezone-aware time operations --- */

const char *tk_time_with_tz(uint64_t ts_ms, const char *tz_name)
{
    if (!tz_name || tz_name[0] == '\0') tz_name = "UTC";

    /* Save/restore TZ to convert in the requested timezone. */
    const char *old_tz = getenv("TZ");
    char *saved_tz = old_tz ? strdup(old_tz) : NULL;
    setenv("TZ", tz_name, 1);
    tzset();

    time_t secs = (time_t)(ts_ms / 1000ULL);
    struct tm tm_info;
    localtime_r(&secs, &tm_info);

    char *buf = malloc(64);
    if (buf) {
        strftime(buf, 64, "%Y-%m-%dT%H:%M:%S%z", &tm_info);
    }

    /* Restore original TZ */
    if (saved_tz) {
        setenv("TZ", saved_tz, 1);
        free(saved_tz);
    } else {
        unsetenv("TZ");
    }
    tzset();

    return buf;
}

int64_t tk_time_utc_offset(const char *tz_name)
{
    if (!tz_name || tz_name[0] == '\0') return 0;

    const char *old_tz = getenv("TZ");
    char *saved_tz = old_tz ? strdup(old_tz) : NULL;
    setenv("TZ", tz_name, 1);
    tzset();

    time_t now = time(NULL);
    struct tm local_tm, utc_tm;
    localtime_r(&now, &local_tm);
    gmtime_r(&now, &utc_tm);

    /* Compute offset = local - UTC in seconds */
    int64_t offset = (int64_t)timegm(&local_tm) - (int64_t)timegm(&utc_tm);

    if (saved_tz) {
        setenv("TZ", saved_tz, 1);
        free(saved_tz);
    } else {
        unsetenv("TZ");
    }
    tzset();

    return offset;
}

const char *tk_time_convert(uint64_t ts_ms, const char *from_tz,
                            const char *to_tz)
{
    /* ts_ms is always UTC internally, so from_tz is informational.
     * We just format in the target timezone. */
    (void)from_tz;
    return tk_time_with_tz(ts_ms, to_tz);
}

/* --- Story 57.11.3: calendar arithmetic --- */

uint64_t tk_time_add_days(uint64_t ts_ms, int64_t n)
{
    return tk_time_add(ts_ms, n * 86400LL * 1000LL);
}

uint64_t tk_time_add_months(uint64_t ts_ms, int64_t n)
{
    TkTimeParts p = tk_time_to_parts(ts_ms);
    int64_t total_months = (int64_t)(p.year * 12 + (p.month - 1)) + n;
    if (total_months < 0) return 0;
    p.year  = (int)(total_months / 12);
    p.month = (int)(total_months % 12) + 1;

    /* Clamp day to the number of days in the target month */
    uint8_t max_day = tk_time_days_in_month(p.year, p.month);
    if (p.day > max_day) p.day = max_day;

    return tk_time_from_parts(p);
}

uint64_t tk_time_add_years(uint64_t ts_ms, int64_t n)
{
    return tk_time_add_months(ts_ms, n * 12);
}

uint64_t tk_time_start_of_day(uint64_t ts_ms)
{
    TkTimeParts p = tk_time_to_parts(ts_ms);
    p.hour = 0; p.min = 0; p.sec = 0;
    return tk_time_from_parts(p);
}

uint64_t tk_time_start_of_month(uint64_t ts_ms)
{
    TkTimeParts p = tk_time_to_parts(ts_ms);
    p.day = 1; p.hour = 0; p.min = 0; p.sec = 0;
    return tk_time_from_parts(p);
}

uint64_t tk_time_start_of_year(uint64_t ts_ms)
{
    TkTimeParts p = tk_time_to_parts(ts_ms);
    p.month = 1; p.day = 1; p.hour = 0; p.min = 0; p.sec = 0;
    return tk_time_from_parts(p);
}

/* --- Story 57.11.4: duration parsing and formatting --- */

TkDurationParseResult tk_time_parse_duration(const char *s)
{
    TkDurationParseResult result;
    memset(&result, 0, sizeof(result));

    if (!s || *s != 'P') {
        result.is_err = 1;
        result.err_msg = "duration must start with 'P'";
        return result;
    }

    const char *p = s + 1;
    int in_time = 0;
    int num = 0;
    int has_num = 0;

    while (*p) {
        if (*p == 'T') {
            in_time = 1;
            p++;
            continue;
        }
        if (*p >= '0' && *p <= '9') {
            num = num * 10 + (*p - '0');
            has_num = 1;
            p++;
            continue;
        }
        if (!has_num) {
            result.is_err = 1;
            result.err_msg = "expected number before designator";
            return result;
        }
        switch (*p) {
        case 'Y': result.ok.years   = num; break;
        case 'D': result.ok.days    = num; break;
        case 'H': result.ok.hours   = num; break;
        case 'S': result.ok.seconds = num; break;
        case 'M':
            if (in_time) result.ok.minutes = num;
            else         result.ok.months  = num;
            break;
        default:
            result.is_err = 1;
            result.err_msg = "unknown designator";
            return result;
        }
        num = 0;
        has_num = 0;
        p++;
    }

    return result;
}

const char *tk_time_format_duration(TkDuration d)
{
    char *buf = malloc(128);
    if (!buf) return NULL;
    char *wp = buf;
    *wp = '\0';

    if (d.years)   wp += sprintf(wp, "%dy ", d.years);
    if (d.months)  wp += sprintf(wp, "%dmo ", d.months);
    if (d.days)    wp += sprintf(wp, "%dd ", d.days);
    if (d.hours)   wp += sprintf(wp, "%dh ", d.hours);
    if (d.minutes) wp += sprintf(wp, "%dm ", d.minutes);
    if (d.seconds) wp += sprintf(wp, "%ds ", d.seconds);

    /* Trim trailing space */
    if (wp > buf && wp[-1] == ' ') wp[-1] = '\0';

    /* Handle zero duration */
    if (buf[0] == '\0') { buf[0] = '0'; buf[1] = 's'; buf[2] = '\0'; }

    return buf;
}

TkDuration tk_time_duration(uint64_t from_ms, uint64_t to_ms)
{
    TkDuration d;
    memset(&d, 0, sizeof(d));

    if (to_ms <= from_ms) return d;

    uint64_t diff_sec = (to_ms - from_ms) / 1000ULL;

    d.days    = (int)(diff_sec / 86400ULL);
    diff_sec %= 86400ULL;
    d.hours   = (int)(diff_sec / 3600ULL);
    diff_sec %= 3600ULL;
    d.minutes = (int)(diff_sec / 60ULL);
    d.seconds = (int)(diff_sec % 60ULL);

    /* Convert days to years+months+days for large spans */
    if (d.days >= 365) {
        d.years = d.days / 365;
        d.days  = d.days % 365;
    }
    if (d.days >= 30) {
        d.months = d.days / 30;
        d.days   = d.days % 30;
    }

    return d;
}

/* --- Story 57.11.5-57.11.7: interplanetary time --- */

double tk_time_julian_date(uint64_t ts_ms)
{
    /* JD = Unix seconds / 86400 + 2440587.5 */
    double unix_days = (double)ts_ms / (1000.0 * 86400.0);
    return unix_days + 2440587.5;
}

double tk_time_mars_sol(uint64_t ts_ms)
{
    /* Mars Sol Date (MSD) as defined by Allison & McEwen (2000).
     * MSD = (JD_TT - 2451549.5) / 1.0274912517 + 44796.0 - 0.00096
     * JD_TT ≈ JD_UTC + 69.184/86400 (TAI-UTC offset for recent dates) */
    double jd_utc = tk_time_julian_date(ts_ms);
    double jd_tt  = jd_utc + 69.184 / 86400.0;
    return (jd_tt - 2451549.5) / 1.0274912517 + 44796.0 - 0.00096;
}

const char *tk_time_format_mars(double sol, const char *fmt)
{
    (void)fmt; /* Simplified: always format as "Sol NNNNN HH:MM:SS MTC" */

    int sol_day = (int)sol;
    double frac = sol - (double)sol_day;
    int total_sec = (int)(frac * 86400.0); /* Mars seconds ≈ Earth seconds */
    int h = total_sec / 3600;
    int m = (total_sec % 3600) / 60;
    int s = total_sec % 60;

    char *buf = malloc(64);
    if (!buf) return NULL;
    snprintf(buf, 64, "Sol %d %02d:%02d:%02d MTC", sol_day, h, m, s);
    return buf;
}

/* Mean orbital distances in AU. Used for approximate light-time. */
static double body_distance_au(const char *body)
{
    if (!body) return 0.0;
    if (strcmp(body, "sun")  == 0) return 0.0;
    if (strcmp(body, "mercury") == 0) return 0.387;
    if (strcmp(body, "venus") == 0) return 0.723;
    if (strcmp(body, "earth") == 0) return 1.0;
    if (strcmp(body, "moon")  == 0) return 1.0; /* effectively Earth distance */
    if (strcmp(body, "mars")  == 0) return 1.524;
    if (strcmp(body, "jupiter") == 0) return 5.203;
    if (strcmp(body, "saturn") == 0) return 9.537;
    return 0.0;
}

double tk_time_light_delay(const char *from_body, const char *to_body,
                           uint64_t ts_ms)
{
    (void)ts_ms; /* Use mean distances; epoch-dependent positions need ephemeris data */

    double d1 = body_distance_au(from_body);
    double d2 = body_distance_au(to_body);
    double dist = (d2 > d1) ? (d2 - d1) : (d1 - d2);

    /* Special case: Earth-Moon distance is about 0.00257 AU */
    if ((strcmp(from_body, "earth") == 0 && strcmp(to_body, "moon") == 0) ||
        (strcmp(from_body, "moon") == 0 && strcmp(to_body, "earth") == 0)) {
        dist = 0.00257;
    }

    /* 1 AU ≈ 499.0 light-seconds */
    return dist * 499.0;
}
