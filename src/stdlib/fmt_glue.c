/*
 * fmt_glue.c — i64-ABI wrappers for the std.fmt module (story 131.30).
 *
 * Arrays arrive as data handles (len at handle[-1], elements i64; str
 * elements are char* stored as i64) — see docs/runtime-abi.md §4 and
 * tk_array.h. bool arrives as i64 0/1. Every wrapper returns a fresh
 * caller-owned NUL-terminated string (or "" on allocation failure).
 *
 * NOTE: fmt.f64 (tk_fmt_f64_w) predates this module — it lives in
 * str_glue.c (102.23), which is always linked, so it is deliberately NOT
 * redefined here (duplicate symbol). Moving it here is a follow-up once
 * str_glue.c is free for edits.
 */

#include "fmt.h"
#include "tk_array.h"
#include <stdint.h>

static int64_t ret_str(char *s) {
    return (int64_t)(intptr_t)(s ? s : "");
}

int64_t tk_fmt_bool_w(int64_t b) {
    return ret_str(fmt_bool(b != 0));
}

int64_t tk_fmt_arr_w(int64_t xs, int64_t sep) {
    return ret_str(fmt_join_i64((const int64_t *)(intptr_t)xs, tk_arr_len(xs),
                                (const char *)(intptr_t)sep));
}

int64_t tk_fmt_strs_w(int64_t xs, int64_t sep) {
    return ret_str(fmt_join_str((const char *const *)(intptr_t)xs, tk_arr_len(xs),
                                (const char *)(intptr_t)sep));
}

int64_t tk_fmt_pad_w(int64_t s, int64_t width, int64_t left) {
    return ret_str(fmt_pad((const char *)(intptr_t)s, width, left != 0));
}
