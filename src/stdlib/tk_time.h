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

#endif /* TK_STDLIB_TK_TIME_H */
