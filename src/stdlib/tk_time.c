/* SPDX-License-Identifier: MIT */
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

/* ── Thread-safe time-zone rules (121.42 (6)) ────────────────────────────
 *
 * time.withtz and time.utcoffset (and time.convert, which calls withtz)
 * used to setenv("TZ", name) + tzset() + localtime_r, then restore TZ; and
 * time.parse had a setenv("TZ","UTC") + mktime fallback where timegm was
 * missing.  The environment is process-wide: another thread's localtime,
 * mktime or getenv("TZ") inside that window saw the borrowed zone, and
 * setenv racing getenv is undefined behaviour.  Nothing here writes the
 * environment or calls tzset now.  A zone is resolved as:
 *   1. a name that is a safe relative path is read as a TZif file
 *      (RFC 8536) under $TZDIR or /usr/share/zoneinfo -- where libc looks;
 *      past its last transition the file's POSIX footer rule applies;
 *   2. otherwise the name is read as a POSIX TZ string ("EST5EDT",
 *      "<+0530>-5:30", "AEST-10AEDT,M10.1.0,M4.1.0/3");
 *   3. otherwise it is UTC, which is what libc did with an unknown TZ.
 * A leading ':' means "file only", as for TZ.  Nothing is cached; every
 * call reads the file again. */

#define TZ_MAX_FILE   (1u << 20)   /* no real TZif file is near 1 MiB */

/* Days since 1970-01-01 of a proleptic-Gregorian date (m 1-12; d may run
 * past the month -- the arithmetic carries it). */
static int64_t tz_days_from_civil(int64_t y, int m, int64_t d)
{
    y -= (m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t mp  = (m > 2) ? (m - 3) : (m + 9);
    int64_t doy = (153 * mp + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

/* timegm without timegm: UTC seconds of broken-down fields, with the month
 * and smaller fields allowed out of range as timegm allows them.  Portable,
 * so no platform needs the old TZ=UTC + mktime fallback. */
static int64_t tz_timegm(const struct tm *t)
{
    int64_t y = (int64_t)t->tm_year + 1900, mon = t->tm_mon;
    y += mon / 12; mon %= 12;
    if (mon < 0) { mon += 12; y--; }
    int64_t days = tz_days_from_civil(y, (int)mon + 1, 1) + (int64_t)t->tm_mday - 1;
    return days * 86400 + (int64_t)t->tm_hour * 3600 + (int64_t)t->tm_min * 60 + t->tm_sec;
}

static int tz_year_of(int64_t t)
{
    time_t s = (time_t)t; struct tm g;
    if (!gmtime_r(&s, &g)) return 1970;
    return g.tm_year + 1900;
}

typedef struct { char kind; int m, w, d, n; int32_t secs; } TzRule;  /* kind J, N, M */
typedef struct {
    int32_t std_off, dst_off;   /* seconds EAST of UTC */
    int     has_dst;
    TzRule  start, end;
} TzPosix;

static const char *tz_pname(const char *p)
{
    if (*p == '<') {
        const char *q = strchr(p, '>');
        return (q && q - p >= 2) ? q + 1 : NULL;
    }
    const char *q = p;
    while ((*q >= 'A' && *q <= 'Z') || (*q >= 'a' && *q <= 'z')) q++;
    return (q - p >= 3) ? q : NULL;
}

/* [+-]h[h][:mm[:ss]] with hours up to `maxh`.  Returns the end, or NULL. */
static const char *tz_ptime(const char *p, int maxh, int32_t *out)
{
    int neg = 0;
    if (*p == '+' || *p == '-') neg = (*p++ == '-');
    if (*p < '0' || *p > '9') return NULL;
    long h = 0, m = 0, s = 0;
    while (*p >= '0' && *p <= '9') { h = h * 10 + (*p++ - '0'); if (h > maxh) return NULL; }
    if (*p == ':') {
        p++;
        if (p[0] < '0' || p[0] > '5' || p[1] < '0' || p[1] > '9') return NULL;
        m = (p[0] - '0') * 10 + (p[1] - '0'); p += 2;
        if (*p == ':') {
            p++;
            if (p[0] < '0' || p[0] > '5' || p[1] < '0' || p[1] > '9') return NULL;
            s = (p[0] - '0') * 10 + (p[1] - '0'); p += 2;
        }
    }
    long v = h * 3600 + m * 60 + s;
    *out = (int32_t)(neg ? -v : v);
    return p;
}

static const char *tz_pnum(const char *p, int lo, int hi, int *out)
{
    if (*p < '0' || *p > '9') return NULL;
    int v = 0;
    while (*p >= '0' && *p <= '9') { v = v * 10 + (*p++ - '0'); if (v > hi) return NULL; }
    if (v < lo) return NULL;
    *out = v;
    return p;
}

static const char *tz_prule(const char *p, TzRule *r)
{
    r->secs = 7200;   /* 02:00 local unless given */
    if (*p == 'J') { r->kind = 'J'; p = tz_pnum(p + 1, 1, 365, &r->n); }
    else if (*p == 'M') {
        r->kind = 'M';
        p = tz_pnum(p + 1, 1, 12, &r->m); if (!p || *p++ != '.') return NULL;
        p = tz_pnum(p, 1, 5, &r->w);      if (!p || *p++ != '.') return NULL;
        p = tz_pnum(p, 0, 6, &r->d);
    } else { r->kind = 'N'; p = tz_pnum(p, 0, 365, &r->n); }
    if (p && *p == '/') p = tz_ptime(p + 1, 167, &r->secs);
    return p;
}

/* Parse a POSIX TZ string.  Returns 0, or -1 when it is not one. */
static int tz_posix_parse(const char *s, TzPosix *z)
{
    memset(z, 0, sizeof *z);
    const char *p = tz_pname(s);
    int32_t off;
    if (!p || !(p = tz_ptime(p, 24, &off))) return -1;
    z->std_off = -off;                       /* POSIX offsets count west */
    if (!*p) return 0;
    const char *q = tz_pname(p);
    if (!q) return -1;
    z->has_dst = 1;
    z->dst_off = z->std_off + 3600;
    p = q;
    if (*p && *p != ',') {
        if (!(p = tz_ptime(p, 24, &off))) return -1;
        z->dst_off = -off;
    }
    if (!*p) {   /* no rule: the US rule, as glibc and the tz code assume */
        z->start.kind = 'M'; z->start.m = 3;  z->start.w = 2; z->start.d = 0; z->start.secs = 7200;
        z->end.kind   = 'M'; z->end.m   = 11; z->end.w   = 1; z->end.d   = 0; z->end.secs   = 7200;
        return 0;
    }
    if (*p++ != ',' || !(p = tz_prule(p, &z->start)) || *p++ != ',' ||
        !(p = tz_prule(p, &z->end)) || *p) return -1;
    return 0;
}

/* Seconds from 00:00 local on Jan 1 of `y` to the rule's transition. */
static int64_t tz_rule_local(int y, const TzRule *r)
{
    int leap = tk_time_is_leap_year(y);
    int64_t yday;
    if (r->kind == 'J') yday = r->n - 1 + (leap && r->n >= 60);
    else if (r->kind == 'N') yday = r->n;
    else {
        int64_t first = tz_days_from_civil(y, r->m, 1);
        int wd1 = (int)((((first % 7) + 7) % 7 + 4) % 7);   /* 1970-01-01: Thursday */
        int day = 1 + (r->d - wd1 + 7) % 7 + (r->w - 1) * 7;
        if (day > tk_time_days_in_month(y, r->m)) day -= 7;
        yday = tz_days_from_civil(y, r->m, day) - tz_days_from_civil(y, 1, 1);
    }
    return yday * 86400 + r->secs;
}

static int32_t tz_posix_offset(const TzPosix *z, int64_t t)
{
    if (!z->has_dst) return z->std_off;
    int y = tz_year_of(t + z->std_off);
    int64_t jan1 = tz_days_from_civil(y, 1, 1) * 86400;
    int64_t s = jan1 + tz_rule_local(y, &z->start) - z->std_off;
    int64_t e = jan1 + tz_rule_local(y, &z->end)   - z->dst_off;
    int in = (s < e) ? (t >= s && t < e) : (t < e || t >= s);
    return in ? z->dst_off : z->std_off;
}

static int64_t tz_be(const unsigned char *p, int n)
{
    uint64_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    if (n == 4) return (int64_t)(int32_t)(uint32_t)v;
    return (int64_t)v;
}

/* A name that may be opened under the zoneinfo directory. */
static int tz_safe_name(const char *n)
{
    size_t len = strlen(n);
    if (len == 0 || len > 255 || n[0] == '/' || strstr(n, "..")) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = n[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '/' || c == '_' || c == '-' ||
              c == '+' || c == '.')) return 0;
    }
    return 1;
}

/* The footer rule of a v2+ TZif file (starting at f), applied at t. */
static int tz_footer_offset(const unsigned char *f, size_t fl, int64_t t, int32_t *off)
{
    if (fl < 2 || f[0] != '\n') return -1;
    const unsigned char *e = memchr(f + 1, '\n', fl - 1);
    char rule[128];
    size_t rl = e ? (size_t)(e - f - 1) : 0;
    TzPosix z;
    if (!e || rl == 0 || rl >= sizeof rule) return -1;
    memcpy(rule, f + 1, rl); rule[rl] = 0;
    if (tz_posix_parse(rule, &z) != 0) return -1;
    *off = tz_posix_offset(&z, t);
    return 0;
}

/* Offset at t from a TZif image.  Returns 0 and sets *off, or -1. */
static int tz_tzif_offset(const unsigned char *b, size_t n, int64_t t, int32_t *off)
{
    if (n < 44 || memcmp(b, "TZif", 4) != 0) return -1;
    int v2 = b[4] >= '2';
    size_t pos = 0;
    int tsz = 4;
    for (int pass = 0; ; pass++) {
        if (pos > n || n - pos < 44 || memcmp(b + pos, "TZif", 4) != 0) return -1;
        const unsigned char *h = b + pos + 20;
        uint64_t isut  = (uint32_t)tz_be(h, 4),      isstd = (uint32_t)tz_be(h + 4, 4);
        uint64_t leap  = (uint32_t)tz_be(h + 8, 4),  timec = (uint32_t)tz_be(h + 12, 4);
        uint64_t typec = (uint32_t)tz_be(h + 16, 4), charc = (uint32_t)tz_be(h + 20, 4);
        uint64_t body = timec * (uint64_t)tsz + timec + typec * 6 + charc +
                        leap * (uint64_t)(tsz + 4) + isstd + isut;
        if (typec == 0 || body > n - pos - 44) return -1;
        const unsigned char *d = b + pos + 44;
        if (v2 && pass == 0) { pos += 44 + (size_t)body; tsz = 8; continue; }
        const unsigned char *times = d, *idx = d + timec * (uint64_t)tsz;
        const unsigned char *types = idx + timec;
        const unsigned char *foot = d + body;
        size_t footlen = n - pos - 44 - (size_t)body;
        size_t ti = 0;   /* RFC 8536: before the first transition, type 0 */
        if (timec > 0 && t >= tz_be(times, tsz)) {
            size_t lo = 0, hi = (size_t)timec - 1;   /* the last transition <= t */
            while (lo < hi) {
                size_t mid = lo + (hi - lo + 1) / 2;
                if (tz_be(times + mid * (size_t)tsz, tsz) <= t) lo = mid; else hi = mid - 1;
            }
            if (lo == timec - 1 && v2 && tz_footer_offset(foot, footlen, t, off) == 0)
                return 0;
            ti = idx[lo];
            if (ti >= typec) return -1;
        } else if (timec == 0 && v2 && tz_footer_offset(foot, footlen, t, off) == 0) {
            return 0;
        }
        *off = (int32_t)tz_be(types + ti * 6, 4);
        return 0;
    }
}

static int tz_file_offset(const char *name, int64_t t, int32_t *off)
{
    if (!tz_safe_name(name)) return -1;
    const char *dir = getenv("TZDIR");   /* read only: nothing here writes it */
    if (!dir || !*dir) dir = "/usr/share/zoneinfo";
    char path[1024];
    int pl = snprintf(path, sizeof path, "%s/%s", dir, name);
    if (pl < 0 || (size_t)pl >= sizeof path) return -1;
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    unsigned char *buf = malloc(TZ_MAX_FILE);
    size_t n = buf ? fread(buf, 1, TZ_MAX_FILE, f) : 0;
    fclose(f);
    int rc = buf ? tz_tzif_offset(buf, n, t, off) : -1;
    free(buf);
    return rc;
}

/* Seconds east of UTC in zone `name` at UTC time t. */
static int32_t tz_offset_at(const char *name, int64_t t)
{
    int32_t off = 0;
    TzPosix z;
    if (!name || !*name) return 0;
    if (name[0] == ':') return tz_file_offset(name + 1, t, &off) == 0 ? off : 0;
    if (tz_file_offset(name, t, &off) == 0) return off;
    if (tz_posix_parse(name, &z) == 0) return tz_posix_offset(&z, t);
    return 0;
}

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
    /* gmtime_r: gmtime's static buffer is shared by every thread */
    struct tm tm_buf;
    struct tm *tm_info = gmtime_r(&secs, &tm_buf);
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

    /* The fields are UTC.  tz_timegm is portable and thread-safe; the old
     * fallback for platforms without timegm set TZ=UTC around mktime
     * (121.42 (6)). */
    int64_t secs = tz_timegm(&tm_info);
    if (secs < 0) {
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
    int64_t secs = tz_timegm(&tm_info);
    if (secs == -1) return 0;
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

    /* 121.42 (6): the zone's own rules, not setenv("TZ") + localtime_r. */
    int64_t secs = (int64_t)(ts_ms / 1000ULL);
    int32_t off  = tz_offset_at(tz_name, secs);
    time_t local = (time_t)(secs + off);
    struct tm tm_info;
    if (!gmtime_r(&local, &tm_info)) return NULL;

    char *buf = malloc(64);
    if (buf) {
        /* %z as strftime prints it from tm_gmtoff: whole minutes, truncated */
        int32_t a = off < 0 ? -off : off;
        size_t w = strftime(buf, 64, "%Y-%m-%dT%H:%M:%S", &tm_info);
        snprintf(buf + w, 64 - w, "%c%02d%02d", off < 0 ? '-' : '+',
                 (int)(a / 3600), (int)((a / 60) % 60));
    }
    return buf;
}

int64_t tk_time_utc_offset(const char *tz_name)
{
    if (!tz_name || tz_name[0] == '\0') return 0;
    /* 121.42 (6): the zone's own rules, not setenv("TZ") + localtime_r. */
    return (int64_t)tz_offset_at(tz_name, (int64_t)time(NULL));
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
    /* 127.246 (6) / 121.53 (3): bounded writes.  Six fields of at most
     * "-2147483648mo " (14 bytes) fit in 128, but snprintf keeps that a
     * property of the code rather than of the arithmetic, and sprintf is
     * deprecated (an error under -Werror) wherever _FORTIFY_SOURCE is 0 --
     * HARDEN=0 and every ASan build. */
    enum { CAP = 128 };
    char *buf = malloc(CAP);
    if (!buf) return NULL;
    size_t n = 0;
    buf[0] = '\0';

    struct { int v; const char *unit; } f[] = {
        { d.years, "y" }, { d.months, "mo" }, { d.days, "d" },
        { d.hours, "h" }, { d.minutes, "m" }, { d.seconds, "s" },
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (!f[i].v || n >= CAP) continue;
        int w = snprintf(buf + n, CAP - n, "%d%s ", f[i].v, f[i].unit);
        if (w < 0) break;
        n += (size_t)w < CAP - n ? (size_t)w : CAP - n - 1;
    }

    /* Trim trailing space */
    if (n > 0 && buf[n - 1] == ' ') buf[n - 1] = '\0';

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
