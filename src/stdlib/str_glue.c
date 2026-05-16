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
#include <regex.h>

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

/* str.push — append element to array (same as str_array_append) */
int64_t tk_str_push_w(int64_t arr, int64_t item) {
    return str_array_append(arr, item);
}

/* str.arrayget — get element at index from toke array */
int64_t tk_str_arrayget_w(int64_t arr, int64_t idx) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return 0;
    return ptr[idx];
}

/* str.arraylen — get length of toke array */
int64_t tk_str_arraylen_w(int64_t arr) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    return ptr[-1];
}

/* str.containsre — regex match using POSIX extended regex */
int64_t tk_str_containsre_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    if (!str || !pat) return 0;
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED | REG_NOSUB) != 0) return 0;
    int match = regexec(&re, str, 0, NULL, 0) == 0 ? 1 : 0;
    regfree(&re);
    return match;
}

/* str.i64tof64 — integer to float conversion */
int64_t tk_str_i64tof64_w(int64_t i) {
    double d = (double)i;
    return f64_to_i64(d);
}

/* ── Aliases for type-specific str conversion names (loke uses these) ── */
int64_t tk_str_fromu16_w(int64_t n) { return tk_str_fromi64_w(n); }
int64_t tk_str_fromu32_w(int64_t n) { return tk_str_fromi64_w(n); }
int64_t tk_str_fromu64_w(int64_t n) { return tk_str_fromi64_w(n); }
int64_t tk_str_fromint64_w(int64_t n) { return tk_str_fromi64_w(n); }
int64_t tk_str_fromi32_w(int64_t n) { return tk_str_fromi64_w(n); }
int64_t tk_str_fromf32_w(int64_t n) { return tk_str_fromfloat_w(n); }
int64_t tk_str_frombool_w(int64_t b) {
    return (int64_t)(intptr_t)(b ? "true" : "false");
}
int64_t tk_str_bytelen_w(int64_t s) { return tk_str_len_w(s); }
int64_t tk_str_lastindexof_w(int64_t s, int64_t sub) { return tk_str_lastindex_w(s, sub); }
int64_t tk_str_pad_w(int64_t s, int64_t w, int64_t ch) { return tk_str_padright_w(s, w, ch); }
int64_t tk_str_parsefloat_w(int64_t s) { return tk_str_tofloat_w(s); }
int64_t tk_str_parseint_w(int64_t s) { return tk_str_toint_w(s); }
int64_t tk_str_replaceall_w(int64_t s, int64_t old, int64_t new_) { return tk_str_replace_w(s, old, new_); }
int64_t tk_str_strip_w(int64_t s) { return tk_str_trim_w(s); }
int64_t tk_str_tof64_w(int64_t s) { return tk_str_tofloat_w(s); }
int64_t tk_str_tof32_w(int64_t s) { return tk_str_tofloat_w(s); }
int64_t tk_str_toi32_w(int64_t s) { return tk_str_toint_w(s); }
int64_t tk_str_toi64_w(int64_t s) { return tk_str_toint_w(s); }
int64_t tk_str_toint64_w(int64_t s) { return tk_str_toint_w(s); }
int64_t tk_str_trimws_w(int64_t s) { return tk_str_trim_w(s); }
int64_t tk_str_compare_w(int64_t a, int64_t b) { return tk_str_eq_w(a, b) ? 0 : 1; }
int64_t tk_str_isdigit_w(int64_t s) {
    const char *p = (const char *)(intptr_t)s;
    if (!p || !*p) return 0;
    while (*p) { if (*p < '0' || *p > '9') return 0; p++; }
    return 1;
}
int64_t tk_str_wordcount_w(int64_t s) {
    const char *p = (const char *)(intptr_t)s;
    if (!p) return 0;
    int count = 0, in_word = 0;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\n') in_word = 0;
        else if (!in_word) { in_word = 1; count++; }
        p++;
    }
    return count;
}
int64_t tk_str_splitfirst_w(int64_t s, int64_t sep) {
    const char *str = (const char *)(intptr_t)s;
    const char *sp = (const char *)(intptr_t)sep;
    if (!str || !sp) return s;
    char *pos = strstr((char*)str, sp);
    if (!pos) return s;
    size_t len = (size_t)(pos - str);
    char *result = malloc(len + 1);
    if (!result) return s;
    memcpy(result, str, len);
    result[len] = '\0';
    return (int64_t)(intptr_t)result;
}
int64_t tk_str_splitwhitespace_w(int64_t s) {
    return tk_str_split_w(s, (int64_t)(intptr_t)" ");
}
/* tk_string_* aliases (loke uses std.string instead of std.str) */
int64_t tk_string_arrayget_w(int64_t a, int64_t i) { return tk_str_arrayget_w(a, i); }
int64_t tk_string_arraylen_w(int64_t a) { return tk_str_arraylen_w(a); }
int64_t tk_string_charat_w(int64_t s, int64_t i) { return tk_str_charat_w(s, i); }
int64_t tk_string_concat_w(int64_t a, int64_t b) { return tk_str_concat_w(a, b); }
int64_t tk_string_contains_w(int64_t s, int64_t sub) { return tk_str_contains_w(s, sub); }
int64_t tk_string_fromi32_w(int64_t n) { return tk_str_fromi64_w(n); }
int64_t tk_string_fromi64_w(int64_t n) { return tk_str_fromi64_w(n); }
int64_t tk_string_len_w(int64_t s) { return tk_str_len_w(s); }
int64_t tk_string_startswith_w(int64_t s, int64_t p) { return tk_str_startswith_w(s, p); }
int64_t tk_string_substr_w(int64_t s, int64_t a, int64_t b) { return tk_str_slice_w(s, a, b); }
int64_t tk_string_trim_w(int64_t s) { return tk_str_trim_w(s); }

/* ── New implementations for linker-gaps ────────────────────────────────── */

/* str.hexid(n) — generate n random bytes as hex (2*n chars).
 * Delegates to crypto_randombytes + crypto_to_hex. */
int64_t tk_str_hexid_w(int64_t n) {
    if (n <= 0) return (int64_t)(intptr_t)"";
    /* Import from crypto_glue: tk_crypto_randomhex_w */
    extern int64_t tk_crypto_randomhex_w(int64_t);
    return tk_crypto_randomhex_w(n);
}

/* str.randhex(n) — alias for hexid */
int64_t tk_str_randhex_w(int64_t n) { return tk_str_hexid_w(n); }

/* string.uuid() ��� generate 16 random bytes as hex (32 hex chars) */
int64_t tk_string_uuid_w(int64_t dummy) { (void)dummy; return tk_str_hexid_w(16); }

/* str.nowiso8601() — current time as ISO 8601 string */
int64_t tk_str_nowiso8601_w(int64_t dummy) {
    (void)dummy;
    extern int64_t tk_time_now_w(void);
    extern int64_t tk_time_format_w(int64_t, int64_t);
    int64_t ts = tk_time_now_w();
    return tk_time_format_w(ts, (int64_t)(intptr_t)"%Y-%m-%dT%H:%M:%SZ");
}

/* str.nowunix() — current Unix timestamp as string */
int64_t tk_str_nowunix_w(int64_t dummy) {
    (void)dummy;
    extern int64_t tk_time_now_w(void);
    int64_t ms = tk_time_now_w();
    int64_t secs = ms / 1000;
    return tk_str_fromi64_w(secs);
}

/* str.containsignorecase(s, sub) — case-insensitive contains */
int64_t tk_str_containsignorecase_w(int64_t s, int64_t sub) {
    const char *str = (const char *)(intptr_t)s;
    const char *needle = (const char *)(intptr_t)sub;
    if (!str || !needle) return 0;
    size_t slen = strlen(str);
    size_t nlen = strlen(needle);
    if (nlen > slen) return 0;
    if (nlen == 0) return 1;
    for (size_t i = 0; i <= slen - nlen; i++) {
        size_t j = 0;
        while (j < nlen) {
            char a = str[i + j], b = needle[j];
            if (a >= 'A' && a <= 'Z') a += 32;
            if (b >= 'A' && b <= 'Z') b += 32;
            if (a != b) break;
            j++;
        }
        if (j == nlen) return 1;
    }
    return 0;
}

/* str.countre(s, pattern) — count regex matches */
int64_t tk_str_countre_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    if (!str || !pat) return 0;
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) return 0;
    int count = 0;
    regmatch_t m;
    const char *p = str;
    while (regexec(&re, p, 1, &m, 0) == 0) {
        count++;
        if (m.rm_eo == 0) { p++; if (!*p) break; continue; }
        p += m.rm_eo;
    }
    regfree(&re);
    return (int64_t)count;
}

/* str.findall(s, pattern) — return array of all regex matches */
int64_t tk_str_findall_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    if (!str || !pat) return 0;
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) return 0;

    /* Collect matches into a dynamic array */
    size_t cap = 16, len = 0;
    int64_t *items = (int64_t *)malloc(cap * sizeof(int64_t));
    if (!items) { regfree(&re); return 0; }

    regmatch_t m;
    const char *p = str;
    while (regexec(&re, p, 1, &m, 0) == 0) {
        size_t mlen = (size_t)(m.rm_eo - m.rm_so);
        char *match = (char *)malloc(mlen + 1);
        if (match) {
            memcpy(match, p + m.rm_so, mlen);
            match[mlen] = '\0';
        }
        if (len == cap) {
            cap *= 2;
            int64_t *tmp = (int64_t *)realloc(items, cap * sizeof(int64_t));
            if (!tmp) break;
            items = tmp;
        }
        items[len++] = (int64_t)(intptr_t)match;
        if (m.rm_eo == 0) { p++; if (!*p) break; continue; }
        p += m.rm_eo;
    }
    regfree(&re);

    /* Build toke array: [len, items...] */
    int64_t *block = (int64_t *)malloc((len + 1) * sizeof(int64_t));
    if (!block) { free(items); return 0; }
    block[0] = (int64_t)len;
    memcpy(block + 1, items, len * sizeof(int64_t));
    free(items);
    return (int64_t)(intptr_t)(block + 1);
}

/* str.matches(s, pattern) — return 1 if entire string matches regex */
int64_t tk_str_matches_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    if (!str || !pat) return 0;
    /* Anchor the pattern: ^(pattern)$ */
    size_t plen = strlen(pat);
    char *anchored = (char *)malloc(plen + 3);
    if (!anchored) return 0;
    anchored[0] = '^';
    memcpy(anchored + 1, pat, plen);
    anchored[plen + 1] = '$';
    anchored[plen + 2] = '\0';
    regex_t re;
    int result = 0;
    if (regcomp(&re, anchored, REG_EXTENDED | REG_NOSUB) == 0) {
        result = (regexec(&re, str, 0, NULL, 0) == 0) ? 1 : 0;
        regfree(&re);
    }
    free(anchored);
    return (int64_t)result;
}

/* str.replacere(s, pattern, replacement) — regex replace all */
int64_t tk_str_replacere_w(int64_t s, int64_t pattern, int64_t replacement) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    const char *rep = (const char *)(intptr_t)replacement;
    if (!str || !pat) return s;
    if (!rep) rep = "";
    regex_t re;
    if (regcomp(&re, pat, REG_EXTENDED) != 0) return s;

    size_t rlen = strlen(rep);
    size_t cap = strlen(str) * 2 + 64;
    char *out = (char *)malloc(cap);
    if (!out) { regfree(&re); return s; }
    size_t opos = 0;
    const char *p = str;
    regmatch_t m;

    while (regexec(&re, p, 1, &m, 0) == 0) {
        /* Copy text before match */
        size_t prefix_len = (size_t)m.rm_so;
        while (opos + prefix_len + rlen + 1 > cap) {
            cap *= 2;
            char *tmp = (char *)realloc(out, cap);
            if (!tmp) { free(out); regfree(&re); return s; }
            out = tmp;
        }
        memcpy(out + opos, p, prefix_len);
        opos += prefix_len;
        memcpy(out + opos, rep, rlen);
        opos += rlen;
        if (m.rm_eo == 0) { out[opos++] = *p; p++; if (!*p) break; continue; }
        p += m.rm_eo;
    }
    /* Copy remaining text */
    size_t tail = strlen(p);
    while (opos + tail + 1 > cap) {
        cap *= 2;
        char *tmp = (char *)realloc(out, cap);
        if (!tmp) { free(out); regfree(&re); return s; }
        out = tmp;
    }
    memcpy(out + opos, p, tail);
    opos += tail;
    out[opos] = '\0';
    regfree(&re);
    return (int64_t)(intptr_t)out;
}

/* str.sha256prefix(s, n) — SHA-256 hash of s, truncated to n hex chars */
int64_t tk_str_sha256prefix_w(int64_t s, int64_t n) {
    extern int64_t tk_crypto_sha256_w(int64_t);
    int64_t hash = tk_crypto_sha256_w(s);
    if (!hash || n <= 0) return hash;
    const char *hex = (const char *)(intptr_t)hash;
    size_t hlen = strlen(hex);
    size_t take = (size_t)n < hlen ? (size_t)n : hlen;
    char *prefix = (char *)malloc(take + 1);
    if (!prefix) return hash;
    memcpy(prefix, hex, take);
    prefix[take] = '\0';
    return (int64_t)(intptr_t)prefix;
}
