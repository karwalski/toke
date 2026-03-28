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

#endif /* TK_STDLIB_TK_TIME_H */
