#ifndef TK_STDLIB_FMT_H
#define TK_STDLIB_FMT_H

/*
 * fmt.h — C interface for the std.fmt standard library module.
 *
 * Value-to-text formatting for printing (story 131.30). Every function
 * returns a fresh heap-allocated NUL-terminated UTF-8 string; the caller
 * owns it (docs/runtime-abi.md §3). Allocation failure returns NULL.
 *
 * The i64-ABI wrappers live in fmt_glue.c. fmt.f64 is served by the
 * pre-existing tk_fmt_f64_w in str_glue.c (102.23) and is not redefined here.
 */

#include <stdint.h>

/* fmt_bool — "true" for non-zero b, "false" otherwise. */
char *fmt_bool(int b);

/* fmt_join_i64 — decimal rendering of xs[0..n-1] joined by sep ("" for n==0). */
char *fmt_join_i64(const int64_t *xs, int64_t n, const char *sep);

/* fmt_join_str — xs[0..n-1] joined by sep ("" for n==0); a NULL element is "". */
char *fmt_join_str(const char *const *xs, int64_t n, const char *sep);

/*
 * fmt_pad — pad s with spaces to `width` code points. left != 0 puts the
 * padding before s (right-aligns the text); left == 0 puts it after.
 * Width is measured in UTF-8 code points, not bytes. If s is already at
 * least `width` wide it is returned unchanged (as a fresh copy).
 */
char *fmt_pad(const char *s, int64_t width, int left);

#endif /* TK_STDLIB_FMT_H */
