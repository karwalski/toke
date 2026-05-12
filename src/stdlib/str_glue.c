/*
 * str_glue.c — i64-ABI wrappers for std.str module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.str.
 */

#include "str.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* --- bitcast helpers for f64 <-> i64 ------------------------------------ */
static double i64_to_f64(int64_t i) { double d; memcpy(&d, &i, sizeof(d)); return d; }
static int64_t f64_to_i64(double d) { int64_t i; memcpy(&i, &d, sizeof(i)); return i; }

int64_t tk_str_concat_w(int64_t a, int64_t b) {
    return (int64_t)(intptr_t)str_concat(
        (const char *)(intptr_t)a,
        (const char *)(intptr_t)b);
}

int64_t tk_str_len_w(int64_t s) {
    return (int64_t)str_len((const char *)(intptr_t)s);
}

int64_t tk_str_trim_w(int64_t s) {
    return (int64_t)(intptr_t)str_trim((const char *)(intptr_t)s);
}

int64_t tk_str_upper_w(int64_t s) {
    return (int64_t)(intptr_t)str_upper((const char *)(intptr_t)s);
}

int64_t tk_str_lower_w(int64_t s) {
    return (int64_t)(intptr_t)str_lower((const char *)(intptr_t)s);
}

int64_t tk_str_from_int(int64_t n) {
    return (int64_t)(intptr_t)str_from_int(n);
}

int64_t tk_str_to_int(int64_t s) {
    if (!s) return 0;
    return (int64_t)atoll((const char *)(intptr_t)s);
}

int64_t tk_str_from_float(int64_t n_as_double_bits) {
    double d;
    memcpy(&d, &n_as_double_bits, sizeof d);
    return (int64_t)(intptr_t)str_from_float(d);
}

/*
 * tk_str_split_w — split a toke string and return a toke-format array.
 *
 * Allocates a block of (len+1) i64 slots:
 *   block[0]     = element count (length)
 *   block[1..N]  = each element as i64 (const char * as integer)
 * Returns (block+1) so that ptr[-1] == length and ptr[i] == element i,
 * matching the toke runtime array layout.
 */
int64_t tk_str_split_w(int64_t s, int64_t sep) {
    if (!s || !sep) return 0;
    StrArray arr = str_split((const char *)(intptr_t)s, (const char *)(intptr_t)sep);
    int64_t *block = (int64_t *)malloc((arr.len + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)arr.len;
    for (uint64_t i = 0; i < arr.len; i++)
        block[i + 1] = (int64_t)(intptr_t)arr.data[i];
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_str_indexof_w(int64_t s, int64_t sub) {
    if (!s || !sub) return -1;
    return str_index((const char *)(intptr_t)s, (const char *)(intptr_t)sub);
}

int64_t tk_str_slice_w(int64_t s, int64_t start, int64_t end_) {
    if (!s) return 0;
    StrSliceResult r = str_slice((const char *)(intptr_t)s, (uint64_t)start, (uint64_t)end_);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_str_replace_w(int64_t s, int64_t old_val, int64_t new_val) {
    if (!s || !old_val) return s;
    return (int64_t)(intptr_t)str_replace(
        (const char *)(intptr_t)s,
        (const char *)(intptr_t)old_val,
        (const char *)(intptr_t)new_val);
}

int64_t tk_str_startswith_w(int64_t s, int64_t prefix) {
    if (!s || !prefix) return 0;
    return (int64_t)str_starts_with((const char *)(intptr_t)s, (const char *)(intptr_t)prefix);
}

int64_t tk_str_endswith_w(int64_t s, int64_t suffix) {
    if (!s || !suffix) return 0;
    return (int64_t)str_ends_with((const char *)(intptr_t)s, (const char *)(intptr_t)suffix);
}

int64_t tk_str_trimprefix_w(int64_t s, int64_t prefix) {
    if (!s || !prefix) return s;
    return (int64_t)(intptr_t)str_trimprefix((const char *)(intptr_t)s, (const char *)(intptr_t)prefix);
}

int64_t tk_str_trimsuffix_w(int64_t s, int64_t suffix) {
    if (!s || !suffix) return s;
    return (int64_t)(intptr_t)str_trimsuffix((const char *)(intptr_t)s, (const char *)(intptr_t)suffix);
}

int64_t tk_str_lastindex_w(int64_t s, int64_t sub) {
    if (!s || !sub) return -1;
    return str_lastindex((const char *)(intptr_t)s, (const char *)(intptr_t)sub);
}

int64_t tk_str_matchbracket_w(int64_t s) {
    if (!s) return 0;
    StrBracketResult r = str_matchbracket((const char *)(intptr_t)s);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

int64_t tk_str_contains_w(int64_t s, int64_t sub) {
    const char *cs = (const char *)(intptr_t)s;
    const char *csub = (const char *)(intptr_t)sub;
    if (!cs || !csub) return 0;
    return str_contains(cs, csub) ? 1 : 0;
}

int64_t tk_str_charat_w(int64_t s, int64_t i) {
    const char *p = s ? (const char *)(intptr_t)s : "";
    int64_t len = (int64_t)strlen(p);
    if (i < 0 || i >= len) return 0;
    return (int64_t)(unsigned char)p[i];
}
int64_t tk_str_substr_w(int64_t s, int64_t start, int64_t end_) {
    return tk_str_slice_w(s, start, end_);
}
int64_t tk_str_padleft_w(int64_t s, int64_t width, int64_t pad) {
    char ch = (char)(pad ? pad : ' ');
    return (int64_t)(intptr_t)str_pad_left(
        (const char *)(intptr_t)s, (uint64_t)width, ch);
}
int64_t tk_str_join_w(int64_t arr, int64_t sep) {
    if (!arr) return (int64_t)(intptr_t)str_join((const char *)(intptr_t)sep, (StrArray){NULL, 0});
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    StrArray parts;
    parts.len = (uint64_t)len;
    parts.data = (const char **)ptr;
    return (int64_t)(intptr_t)str_join((const char *)(intptr_t)sep, parts);
}
int64_t tk_str_toint_w(int64_t s) {
    const char *p = s ? (const char *)(intptr_t)s : "";
    IntParseResult r = str_to_int(p);
    return r.is_err ? 0 : r.ok;
}

/* str extras (tobytes, frombytes, bytes, print) */
int64_t tk_str_tobytes_w(int64_t s) { return s; }
int64_t tk_str_frombytes_w(int64_t b) { return b; }
int64_t tk_str_bytes_w(int64_t s) { return s; }
int64_t tk_str_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}

/* --- empty array helper: allocates a block with count=0, returns block+1 --- */
static int64_t str_make_empty_array(void) {
    int64_t *block = (int64_t *)malloc(sizeof(int64_t));
    if (!block) return 0;
    block[0] = 0;
    return (int64_t)(intptr_t)(block + 1);
}

/* --- array append helper (same layout as tk_array_append_w) ------------- */
static int64_t str_array_append(int64_t arr_i64, int64_t elem) {
    if (!arr_i64) {
        /* create single-element array */
        int64_t *block = (int64_t *)malloc(2 * sizeof(int64_t));
        if (!block) return 0;
        block[0] = 1;
        block[1] = elem;
        return (int64_t)(intptr_t)(block + 1);
    }
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    int64_t *block = (int64_t *)malloc((size_t)(len + 2) * sizeof(int64_t));
    if (!block) return arr_i64;
    block[0] = len + 1;
    if (len > 0) memcpy(block + 1, ptr, (size_t)len * sizeof(int64_t));
    block[len + 1] = elem;
    return (int64_t)(intptr_t)(block + 1);
}

/* str newarray */
int64_t tk_str_newarray_w(int64_t dummy) { (void)dummy; return str_make_empty_array(); }

/* str extras */
int64_t tk_str_emptyarr_w(void) { return str_make_empty_array(); }

int64_t tk_str_repeat_w(int64_t s, int64_t n) {
    return (int64_t)(intptr_t)str_repeat(
        (const char *)(intptr_t)s, (uint64_t)n);
}

int64_t tk_str_reverse_w(int64_t s) {
    return (int64_t)(intptr_t)str_reverse((const char *)(intptr_t)s);
}

int64_t tk_str_format_w(int64_t fmt, int64_t arg) {
    return (int64_t)(intptr_t)str_format(
        (const char *)(intptr_t)fmt, arg);
}

int64_t tk_str_ends_w(int64_t s, int64_t suffix) {
    return tk_str_endswith_w(s, suffix);
}

int64_t tk_str_eq_w(int64_t a, int64_t b) {
    const char *sa = (const char *)(intptr_t)a;
    const char *sb = (const char *)(intptr_t)b;
    if (sa == sb) return 1;
    if (!sa || !sb) return 0;
    return strcmp(sa, sb) == 0 ? 1 : 0;
}

int64_t tk_str_append_w(int64_t s, int64_t t) {
    return tk_str_concat_w(s, t);
}

int64_t tk_str_newarr_w(void) { return str_make_empty_array(); }

int64_t tk_str_padright_w(int64_t s, int64_t width, int64_t pad) {
    char ch = (char)(pad ? pad : ' ');
    return (int64_t)(intptr_t)str_pad_right(
        (const char *)(intptr_t)s, (uint64_t)width, ch);
}

int64_t tk_str_tolower_w(int64_t s) {
    return tk_str_lower_w(s);
}

int64_t tk_str_arrappend_w(int64_t arr, int64_t item) {
    return str_array_append(arr, item);
}

int64_t tk_str_arrayappend_w(int64_t arr, int64_t item) {
    return str_array_append(arr, item);
}

int64_t tk_str_toupper_w(int64_t s) {
    return tk_str_upper_w(s);
}

int64_t tk_str_appendarray_w(int64_t arr, int64_t item) {
    return str_array_append(arr, item);
}

int64_t tk_str_arraypush_w(int64_t arr, int64_t item) {
    return str_array_append(arr, item);
}

int64_t tk_str_starts_w(int64_t s, int64_t prefix) {
    return tk_str_startswith_w(s, prefix);
}

int64_t tk_str_appenditem_w(int64_t arr, int64_t item) {
    return str_array_append(arr, item);
}

int64_t tk_str_emptylist_w(void) { return str_make_empty_array(); }

int64_t tk_str_replaceitem_w(int64_t arr, int64_t idx, int64_t val) {
    if (!arr) return arr;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return arr;
    /* copy-on-write: allocate new block */
    int64_t *block = (int64_t *)malloc((size_t)(len + 1) * sizeof(int64_t));
    if (!block) return arr;
    block[0] = len;
    memcpy(block + 1, ptr, (size_t)len * sizeof(int64_t));
    block[idx + 1] = val;
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_str_arrof_w(int64_t v) {
    /* create a single-element array */
    int64_t *block = (int64_t *)malloc(2 * sizeof(int64_t));
    if (!block) return 0;
    block[0] = 1;
    block[1] = v;
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_str_fromfloat_w(int64_t f) {
    return (int64_t)(intptr_t)str_from_float(i64_to_f64(f));
}

int64_t tk_str_tofloat_w(int64_t s) {
    if (!s) return f64_to_i64(0.0);
    FloatParseResult r = str_to_float((const char *)(intptr_t)s);
    return r.is_err ? f64_to_i64(0.0) : f64_to_i64(r.ok);
}

int64_t tk_str_fromf64_w(int64_t f) {
    return (int64_t)(intptr_t)str_from_float(i64_to_f64(f));
}

int64_t tk_str_fromi64_w(int64_t i) {
    return (int64_t)(intptr_t)str_from_int(i);
}

int64_t tk_str_isempty_w(int64_t s) {
    const char *p = (const char *)(intptr_t)s;
    return (!p || *p == '\0') ? 1 : 0;
}

int64_t tk_str_parsef64_w(int64_t s) {
    if (!s) return f64_to_i64(0.0);
    FloatParseResult r = str_to_float((const char *)(intptr_t)s);
    return r.is_err ? f64_to_i64(0.0) : f64_to_i64(r.ok);
}

int64_t tk_str_emptyof_w(int64_t type) {
    (void)type;
    return str_make_empty_array();
}

int64_t tk_str_setat_w(int64_t s, int64_t idx, int64_t ch) {
    const char *p = (const char *)(intptr_t)s;
    if (!p) return s;
    size_t len = strlen(p);
    if (idx < 0 || (size_t)idx >= len) return s;
    char *out = malloc(len + 1);
    if (!out) return s;
    memcpy(out, p, len + 1);
    out[idx] = (char)ch;
    return (int64_t)(intptr_t)out;
}
