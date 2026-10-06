/* SPDX-License-Identifier: MIT */
/*
 * str_glue.c — i64-ABI wrappers for std.str module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.str.
 */

#include "str.h"
#include "bytes_rt.h"   /* Stage 5: [byte] pack/unpack marshalling */
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "tk_regex.h" /* 137.9/121.35: the one pattern language (no POSIX regex.h) */

/* 114.53/114.54: the string→number parse wrappers signal parse failure via
 * tk_current_error (0 = ok, else a ParseErr box -- 127.241) and always return
 * the real value — the 0
 * sentinel can't distinguish a legitimately-parsed 0/0.0 from failure. The
 * match-decode discriminates ok/err on this flag for those calls. */
/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

/* 127.241: the per-thread error box (tk_runtime.c, 127.109). */
void *tk_err_box(int64_t nbytes);

/*
 * str_raise — 127.241.  Fail a `T!SliceErr` / `T!ParseErr` / `T!EncodingErr`
 * / `T!BracketErr` call with a payload of the declared shape.
 *
 * All four error types are the record {msg:str} (stdlib/str.tki), so the box
 * is one word, the message pointer, and `mt f(){$err:e e.msg}` reads it
 * (runtime-abi.md §7.3, "a record: a tk_err_box of the record's layout").
 * Before this, slice / matchbracket / frombytes returned a bare 0 and never
 * set the slot (the bare-glue list in stdlib_tables.c), and the parse
 * wrappers stored the payload-less 1 -- so `e.msg` on a failed str.toint was
 * a nil dereference (RT005) although the .tki declares the field.  Returns
 * 0 so a caller can `return str_raise(...)` for a pointer-valued result.
 */
static int64_t str_raise(const char *msg) {
    const char **box = (const char **)tk_err_box((int64_t)sizeof(const char *));
    box[0] = msg ? msg : "";
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

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
    int64_t h = tk_arr_alloc((int64_t)arr.len, (int64_t)arr.len);
    if (!h) return 0;
    int64_t *out = (int64_t *)(intptr_t)h;
    for (uint64_t i = 0; i < arr.len; i++)
        out[i] = (int64_t)(intptr_t)arr.data[i];
    return h;
}

/*
 * str.fields(s) — 116.6/A6: split on runs of ASCII whitespace (space/tab/CR/LF),
 * dropping leading/trailing/empty fields (like Go's strings.Fields). This is the
 * idiomatic tokeniser for whitespace-separated input — the corpus hand-rolls it
 * as a per-char scan loop because `str.split(s;" ")` yields empty fields on
 * whitespace runs. Returns a toke [str].
 */
static int tk_str_is_ws(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
int64_t tk_str_fields_w(int64_t s) {
    const char *str = (const char *)(intptr_t)s;
    if (!str) return tk_arr_alloc(0, 0);
    int64_t n = 0; int in = 0;
    for (const char *p = str; *p; p++) {
        if (tk_str_is_ws(*p)) in = 0;
        else { if (!in) n++; in = 1; }
    }
    int64_t h = tk_arr_alloc(n, n);
    if (!h) return 0;
    int64_t *out = (int64_t *)(intptr_t)h;
    int64_t idx = 0;
    const char *p = str;
    while (*p) {
        while (*p && tk_str_is_ws(*p)) p++;
        if (!*p) break;
        const char *start = p;
        while (*p && !tk_str_is_ws(*p)) p++;
        size_t len = (size_t)(p - start);
        char *field = (char *)malloc(len + 1);
        if (!field) { out[idx++] = (int64_t)(intptr_t)""; continue; }
        memcpy(field, start, len); field[len] = '\0';
        out[idx++] = (int64_t)(intptr_t)field;
    }
    return h;
}

int64_t tk_str_indexof_w(int64_t s, int64_t sub) {
    if (!s || !sub) return -1;
    return str_index((const char *)(intptr_t)s, (const char *)(intptr_t)sub);
}

/* 127.91: str.indexfrom(s; sub; start) — str.indexof with a start offset.
 * Returns the byte offset (from the start of `s`, not of the search) of the
 * first occurrence of `sub` at or after byte `start`, or -1.  A negative start
 * searches from 0; a start past the end of `s` finds nothing (-1); a start
 * equal to the length finds only the empty string.
 *
 * Without it a scanner must re-slice the tail before every search, which
 * allocates and copies O(remaining) bytes per token and never frees them.
 *
 * The bounds check is O(start): a toke string is a bare NUL-terminated
 * buffer with no length header, so the only way to know `start` lies inside
 * `s` is to confirm no NUL precedes it.  memchr does that at memory
 * bandwidth and allocates nothing; C11 7.24.5.1 requires it to stop at the
 * first match, so it never reads past the terminator. */
int64_t tk_str_indexfrom_w(int64_t s, int64_t sub, int64_t start) {
    if (!s || !sub) return -1;
    const char *cs = (const char *)(intptr_t)s;
    if (start < 0) start = 0;
    if (start > 0 && memchr(cs, '\0', (size_t)start)) return -1;
    const char *p = strstr(cs + start, (const char *)(intptr_t)sub);
    return p ? (int64_t)(p - cs) : -1;
}

int64_t tk_str_slice_w(int64_t s, int64_t start, int64_t end_) {
    if (!s) return str_raise("str.slice: null string");
    StrSliceResult r = str_slice((const char *)(intptr_t)s, (uint64_t)start, (uint64_t)end_);
    if (r.is_err) return str_raise(r.err_msg);                  /* 127.241 */
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok;
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
    if (!s) return str_raise("str.matchbracket: null string");
    StrBracketResult r = str_matchbracket((const char *)(intptr_t)s);
    if (r.is_err) return str_raise(r.err_msg);                  /* 127.241 */
    tk_current_error = 0;
    return (int64_t)(intptr_t)r.ok;
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
/* 138.11 — an i64 where a str is declared.  The checker does not yet reject
 * an integer passed to a str parameter of a stdlib call (reported to GA), so
 * `str.tof32(12345)` dereferenced address 12345 (loke T-36, rc 139) and
 * `str.isdigit(str.charat(s;0))` a byte code (T-28, SIGSEGV).  No value below
 * 65536 is a valid pointer on a supported platform (Linux's default
 * vm.mmap_min_addr; macOS reserves the low 4 GiB), so such an argument is
 * known not to be a string: the parse wrappers fail with the error slot set
 * instead of crashing, and the byte predicates read it as the byte it is.
 * This is damage limitation only -- a larger integer still reaches the
 * dereference; the cure is the checker rejecting the call. */
#define TK_NOT_A_STR(v) ((uint64_t)(v) < 65536u)

/* 138.11: the pad is a byte code (0 = space).  A value that is not a byte
 * is a str the checker let through (loke T-36/T-12 passed "0" and "*"): its
 * first byte is the pad, where this used to pad with the pointer's low byte. */
static char pad_byte(int64_t pad) {
    if (pad == 0) return ' ';
    if (pad > 0 && pad < 256) return (char)pad;
    if (TK_NOT_A_STR(pad)) return ' ';
    const char *p = (const char *)(intptr_t)pad;
    return *p ? *p : ' ';
}

int64_t tk_str_padleft_w(int64_t s, int64_t width, int64_t pad) {
    char ch = pad_byte(pad);
    return (int64_t)(intptr_t)str_pad_left(
        (const char *)(intptr_t)s, (uint64_t)width, ch);
}
/* 114.6: str.join(sep:str; arr:[str]) — params are (sep, arr) per str.tki; the
 * wrapper previously took (arr, sep), so a real `str.join(",";@(...))` passed
 * the separator where the array was expected and read ptr[-1] off a string →
 * SIGSEGV. Order now matches the .tki. */
int64_t tk_str_join_w(int64_t sep, int64_t arr) {
    if (!arr) return (int64_t)(intptr_t)str_join((const char *)(intptr_t)sep, (StrArray){NULL, 0});
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    StrArray parts;
    parts.len = (uint64_t)len;
    parts.data = (const char **)ptr;
    return (int64_t)(intptr_t)str_join((const char *)(intptr_t)sep, parts);
}
int64_t tk_str_toint_w(int64_t s) {
    /* 114.54: the integer 0 collides with the error-union's 0 sentinel, so
     * str.toint("0") was misread as a parse error. Signal failure via
     * tk_current_error and return the real value (see 114.53 for f64). */
    const char *p = s ? (const char *)(intptr_t)s : "";
    IntParseResult r = str_to_int(p);
    if (r.is_err) return str_raise(r.err_msg);                  /* 127.241: ParseErr{msg} */
    tk_current_error = 0;
    return r.ok;
}

/* str extras (tobytes, frombytes, bytes, print) — Stage 5: real [byte] rep.
 * str.bytes(s)  -> [byte] (i64-array of s's UTF-8 byte values, binary-safe).
 * str.frombytes -> str    (NUL-terminated C string from the byte values; an
 *                          interior 0x00 truncates the str — an inherent limit
 *                          of the char* str rep until length-prefixed str). */
int64_t tk_str_bytes_w(int64_t s) {
    if (!s) return tk_bytes_pack((const uint8_t *)"", 0);
    const char *str = (const char *)(intptr_t)s;
    return tk_bytes_pack((const uint8_t *)str, (uint64_t)strlen(str));
}
int64_t tk_str_tobytes_w(int64_t s) { return tk_str_bytes_w(s); }
/* 127.241: str!EncodingErr, as declared and as str.md documents -- bytes that
 * are not valid UTF-8 fail with EncodingErr{msg}.  The value channel still
 * carries the decoded bytes on that path (it always did, and a caller that
 * does not match on the result keeps getting them); only the slot is new. */
int64_t tk_str_frombytes_w(int64_t b) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(b, &buf);
    if (!buf) { str_raise("str.frombytes: out of memory"); return (int64_t)(intptr_t)""; }
    ByteArray ba = { buf, n };
    StrEncResult v = str_from_bytes(ba);
    if (v.is_err) str_raise(v.err_msg);
    else { free((void *)v.ok); tk_current_error = 0; }
    return (int64_t)(intptr_t)buf;  /* buf is already NUL-terminated by unpack */
}
int64_t tk_str_print_w(int64_t s) {
    if (s) printf("%s", (const char *)(intptr_t)s);
    return 0;
}

/* --- empty array helper: allocates a block with count=0, returns block+1 --- */
static int64_t str_make_empty_array(void) {
    return tk_arr_alloc(0, 0);
}

/* --- array append helper (same layout as tk_array_append_w) ------------- */
static int64_t str_array_append(int64_t arr_i64, int64_t elem) {
    int64_t len = tk_arr_len(arr_i64);
    int64_t h = tk_arr_alloc(len + 1, len + 1);
    if (!h) return arr_i64;
    int64_t *out = (int64_t *)(intptr_t)h;
    if (len > 0) memcpy(out, (int64_t *)(intptr_t)arr_i64, (size_t)len * sizeof(int64_t));
    out[len] = elem;
    return h;
}

/* str newarray */
int64_t tk_str_newarray_w(void) { return str_make_empty_array(); }

/* str extras */
int64_t tk_str_emptyarr_w(void) { return str_make_empty_array(); }

int64_t tk_str_repeat_w(int64_t s, int64_t n) {
    return (int64_t)(intptr_t)str_repeat(
        (const char *)(intptr_t)s, (uint64_t)n);
}

int64_t tk_str_reverse_w(int64_t s) {
    return (int64_t)(intptr_t)str_reverse((const char *)(intptr_t)s);
}

int64_t tk_str_format_w(int64_t val, int64_t fmt) {
    /*
     * ABI: s.format(value, fmt_string) — the compiler emits value as arg1,
     * format string as arg2.  The underlying str_format(fmt, arg) expects
     * the format string first, so we swap here.
     *
     * The value may be an i64 integer, an f64 encoded as i64 bits, or a
     * string pointer — str_format inspects the format specifier to decide.
     */
    return (int64_t)(intptr_t)str_format(
        (const char *)(intptr_t)fmt, val);
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
    char ch = pad_byte(pad);
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

/* Forward declaration */
int64_t tk_str_replaceitem_w(int64_t arr, int64_t idx, int64_t val);

/* str.set(arr, idx, val) — set element at index in array (copy-on-write) */
int64_t tk_str_set_w(int64_t arr, int64_t idx, int64_t val) {
    return tk_str_replaceitem_w(arr, idx, val);
}

int64_t tk_str_replaceitem_w(int64_t arr, int64_t idx, int64_t val) {
    if (!arr) return arr;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return arr;
    /* copy-on-write: allocate new block */
    int64_t h = tk_arr_alloc(len, len);
    if (!h) return arr;
    int64_t *out = (int64_t *)(intptr_t)h;
    memcpy(out, ptr, (size_t)len * sizeof(int64_t));
    out[idx] = val;
    return h;
}

int64_t tk_str_arrof_w(int64_t v) {
    /* create a single-element array */
    int64_t h = tk_arr_alloc(1, 1);
    if (!h) return 0;
    ((int64_t *)(intptr_t)h)[0] = v;
    return h;
}

int64_t tk_str_fromfloat_w(int64_t f) {
    return (int64_t)(intptr_t)str_from_float(i64_to_f64(f));
}


int64_t tk_str_tofloat_w(int64_t s) {
    if (TK_NOT_A_STR(s)) { str_raise("argument is not a str"); return f64_to_i64(0.0); }
    FloatParseResult r = str_to_float((const char *)(intptr_t)s);
    if (r.is_err) { str_raise(r.err_msg); return f64_to_i64(0.0); }   /* 127.241: ParseErr{msg} */
    tk_current_error = 0;
    return f64_to_i64(r.ok);
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
    if (TK_NOT_A_STR(s)) { str_raise("argument is not a str"); return f64_to_i64(0.0); }
    FloatParseResult r = str_to_float((const char *)(intptr_t)s);
    if (r.is_err) { str_raise(r.err_msg); return f64_to_i64(0.0); }   /* 127.241: ParseErr{msg} */
    tk_current_error = 0;
    return f64_to_i64(r.ok);
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

/* str.get — get element at index (compiler resolves p.get(i) to this) */
int64_t tk_str_get_w(int64_t arr, int64_t idx) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return 0;
    return ptr[idx];
}

/* tk_array_set_w — issue 112.3
 * Replace element at index. Returns a NEW array (immutable-style), mirroring
 * how tk_array_append_w in collections_glue.c builds a fresh allocation.
 * Previously `.set` was unconditionally routed to tk_map_set_w, which casts
 * the numeric index to a string pointer and crashes via strcmp(). */
int64_t tk_array_set_w(int64_t arr_i64, int64_t idx, int64_t elem) {
    if (!arr_i64) return arr_i64;
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return arr_i64;
    int64_t h = tk_arr_alloc(len, len);
    if (!h) return arr_i64;
    int64_t *out = (int64_t *)(intptr_t)h;
    if (len > 0) memcpy(out, ptr, (size_t)len * sizeof(int64_t));
    out[idx] = elem;
    return h;
}

/*
 * tk_array_set_inplace_w — set assuming the caller uniquely owns `arr`
 * (ADR-0006 D2). Emitted by codegen only at a self-update `x = x.set(i;v)`
 * proven linearly-owned, turning an O(N) loop of sets from O(N^2) into O(N).
 */
int64_t tk_array_set_inplace_w(int64_t arr_i64, int64_t idx, int64_t elem) {
    if (!arr_i64) return arr_i64;
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return arr_i64;
    ptr[idx] = elem;
    return arr_i64;
}

/* str.arraylen — get length of toke array */
int64_t tk_str_arraylen_w(int64_t arr) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    return ptr[-1];
}

/* ── regex entry points (137.9, 121.35) ────────────────────────────────
 * One engine (tk_regex.h), one language, and two loud failures in place of
 * the silent "no match" regcomp failure used to produce:
 *   RT008  the pattern is not in the language (the compiler reports a
 *          literal pattern as E4075 at --check; this catches computed ones)
 *   RT009  a call spent its step budget (tkre_budget: 8 full passes over the
 *          subject plus a fixed 2^26 steps) -- only the find-next loops of
 *          countre/findall/replacere can, by re-scanning the tail
 * Both print to stderr and exit(1), like the other RT traps
 * (docs/runtime-abi.md section 9). */
static void regex_trap_invalid(const char *fn, const char *pat, const TkReErr *e) {
    fprintf(stderr, "RT008: invalid regex pattern in str.%s: %s (at byte %d of \"%.80s%s\")\n",
            fn, e->msg, e->pos, pat, strlen(pat) > 80 ? "..." : "");
    exit(1);
}

static void regex_trap_budget(const char *fn, const char *pat, size_t len) {
    fprintf(stderr, "RT009: regex step budget exceeded in str.%s (pattern \"%.80s%s\", subject %zu bytes)\n",
            fn, pat, strlen(pat) > 80 ? "..." : "", len);
    exit(1);
}

static void regex_compile_or_trap(const char *fn, const char *pat, TkRe *re) {
    TkReErr e;
    if (tkre_compile(re, pat, &e) != 0) regex_trap_invalid(fn, pat, &e);
}

static int64_t regex_find1(const char *fn, const char *str, const char *pat, int full) {
    if (!str || !pat) return 0;
    TkRe re;
    regex_compile_or_trap(fn, pat, &re);
    size_t len = strlen(str), ms, me;
    uint64_t budget = tkre_budget(&re, len);
    int rc = tkre_search(&re, str, len, 0, full, &budget, &ms, &me);
    tkre_free(&re);
    if (rc == TKRE_BUDGET) regex_trap_budget(fn, pat, len);
    if (rc == TKRE_NOMEM) { fprintf(stderr, "str.%s: out of memory\n", fn); exit(1); }
    return rc == TKRE_FOUND ? 1 : 0;
}

/* Find-next over one subject with one budget for the whole call.  After an
 * empty match the next search starts one byte on, and an empty match right
 * where the previous match ended is skipped (RE2/Go rule), so "x*" over "abc"
 * matches at 0, 1, 2 and 3. */
typedef struct {
    const TkRe *re; const char *s; size_t len, from, ms, me;
    uint64_t budget; int prev_end_valid; size_t prev_end;
} RegexIter;

static void regex_iter_init(RegexIter *it, const TkRe *re, const char *s) {
    memset(it, 0, sizeof *it);
    it->re = re; it->s = s; it->len = strlen(s);
    it->budget = tkre_budget(re, it->len);
}

static int regex_next(const char *fn, RegexIter *it, const char *pat) {
    while (it->from <= it->len) {
        size_t ms, me;
        int rc = tkre_search(it->re, it->s, it->len, it->from, 0, &it->budget, &ms, &me);
        if (rc == TKRE_BUDGET) regex_trap_budget(fn, pat, it->len);
        if (rc == TKRE_NOMEM) { fprintf(stderr, "str.%s: out of memory\n", fn); exit(1); }
        if (rc != TKRE_FOUND) return 0;
        if (ms == me && it->prev_end_valid && ms == it->prev_end) {
            it->from = ms + 1;           /* empty match glued to the last one */
            continue;
        }
        it->ms = ms; it->me = me;
        it->from = me > ms ? me : me + 1;
        it->prev_end_valid = 1; it->prev_end = me;
        return 1;
    }
    return 0;
}

/* str.containsre — 1 if the pattern matches anywhere in s (tk_regex.h). */
int64_t tk_str_containsre_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    return regex_find1("containsre", str, pat, 0);
}

/* str.checkre(pattern) -> str!RegexErr  (121.45 item 1)
 * The error-as-value door into the pattern language.  Every other regex
 * call reads an invalid computed pattern as RT008 and exits, so a server
 * that takes a pattern from a request had no way to refuse it and keep
 * running.  This compiles the pattern with the same tkre_compile the five
 * matching calls use -- so "checkre accepted it" and "they will not trap
 * RT008 on it" are the same statement -- and returns the pattern unchanged
 * on success, or a RegexErr {msg; pos} record: the engine's reason and the
 * byte offset into the pattern (-1 when the failure is not at a byte, e.g.
 * out of memory).  The message lives in tk_err_msg's per-thread ring
 * (tk_runtime.c, 127.102), because TkReErr.msg is on this stack frame.
 * Never RT008, and never E4075 at --check: returning the error is the point. */
const char *tk_err_msg(const char *fmt, ...);
static int64_t regex_raise(const char *msg, int64_t pos) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)(msg ? msg : "");
    box[1] = pos;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

int64_t tk_str_checkre_w(int64_t pattern) {
    const char *pat = (const char *)(intptr_t)pattern;
    if (!pat) return regex_raise("str.checkre: null pattern", -1);
    TkRe re;
    TkReErr e;
    if (tkre_compile(&re, pat, &e) != 0)
        return regex_raise(tk_err_msg("%s", e.msg), (int64_t)e.pos);
    tkre_free(&re);
    tk_current_error = 0;
    return pattern;
}

/* str.i64tof64 — integer to float conversion */
int64_t tk_str_i64tof64_w(int64_t i) {
    double d = (double)i;
    return f64_to_i64(d);
}

/* ── Aliases for type-specific str conversion names ─────────────────────
 * 136.45 (owner decision 2026-09-22: withdraw, do not promote): this block
 * held wrappers that existed only because one downstream project (loke)
 * called them.  Withdrawn: fromu16 fromu32 fromu64 fromint64 fromi32 fromf32
 * (-> str.fromint / str.fromfloat), bytelen (-> str.len), pad (-> str.padright),
 * strip trimws (-> str.trim), splitwhitespace (-> str.split(s;" ")),
 * splitfirst and wordcount (no alias; compose from str.split / str.index).
 * Kept, because a later story published them with a type, a doc and a test
 * (138.11, C313): frombool replaceall compare isdigit, and tof32/tof64/
 * parsefloat above.  Kept for another batch's decision: toi32 toi64 toint64
 * parseint (aliases of toint, which 127.30 retypes). */
int64_t tk_str_frombool_w(int64_t b) {
    return (int64_t)(intptr_t)(b ? "true" : "false");
}
int64_t tk_str_lastindexof_w(int64_t s, int64_t sub) { return tk_str_lastindex_w(s, sub); }
int64_t tk_str_parsefloat_w(int64_t s) { return tk_str_tofloat_w(s); }
int64_t tk_str_parseint_w(int64_t s) { return tk_str_toint_w(s); }
int64_t tk_str_replaceall_w(int64_t s, int64_t old, int64_t new_) { return tk_str_replace_w(s, old, new_); }
int64_t tk_str_tof64_w(int64_t s) { return tk_str_tofloat_w(s); }
int64_t tk_str_tof32_w(int64_t s) { return tk_str_tofloat_w(s); }
int64_t tk_str_toi32_w(int64_t s) { return tk_str_toint_w(s); }
int64_t tk_str_toi64_w(int64_t s) { return tk_str_toint_w(s); }
int64_t tk_str_toint64_w(int64_t s) { return tk_str_toint_w(s); }
/* str.compare(a, b) — 138.11: strcmp order, -1 / 0 / 1.  This returned
 * "equal ? 0 : 1", so compare(a,b) and compare(b,a) were both 1 and nothing
 * could sort with it (loke T-36). */
int64_t tk_str_compare_w(int64_t a, int64_t b) {
    const char *x = a ? (const char *)(intptr_t)a : "";
    const char *y = b ? (const char *)(intptr_t)b : "";
    int c = strcmp(x, y);
    return c < 0 ? -1 : c > 0 ? 1 : 0;
}
int64_t tk_str_isdigit_w(int64_t s) {
    if (TK_NOT_A_STR(s)) return (s >= '0' && s <= '9') ? 1 : 0;   /* 138.11: a byte code */
    const char *p = (const char *)(intptr_t)s;
    if (!*p) return 0;
    while (*p) { if (*p < '0' || *p > '9') return 0; p++; }
    return 1;
}
/* 136.45: the tk_string_* aliases (std.string, a second name for std.str
 * that one downstream project imported) are withdrawn with the module. */

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


/* str.nowiso8601() — current time as ISO 8601 string */
int64_t tk_str_nowiso8601_w(void) {
    extern int64_t tk_time_now_w(void);
    extern int64_t tk_time_format_w(int64_t, int64_t);
    int64_t ts = tk_time_now_w();
    return tk_time_format_w(ts, (int64_t)(intptr_t)"%Y-%m-%dT%H:%M:%SZ");
}

/* str.nowunix() — current Unix timestamp as string */
int64_t tk_str_nowunix_w(void) {
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

/* str.countre(s, pattern) — number of non-overlapping matches. */
int64_t tk_str_countre_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    if (!str || !pat) return 0;
    TkRe re;
    regex_compile_or_trap("countre", pat, &re);
    RegexIter it; regex_iter_init(&it, &re, str);
    int64_t count = 0;
    while (regex_next("countre", &it, pat)) count++;
    tkre_free(&re);
    return count;
}

/* str.findall(s, pattern) — array of every non-overlapping match. */
int64_t tk_str_findall_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    if (!str || !pat) return 0;
    TkRe re;
    regex_compile_or_trap("findall", pat, &re);
    size_t cap = 16, len = 0;
    int64_t *items = (int64_t *)malloc(cap * sizeof(int64_t));
    if (!items) { tkre_free(&re); return 0; }
    RegexIter it; regex_iter_init(&it, &re, str);
    while (regex_next("findall", &it, pat)) {
        size_t mlen = it.me - it.ms;
        char *match = (char *)malloc(mlen + 1);
        if (!match) break;
        memcpy(match, str + it.ms, mlen);
        match[mlen] = '\0';
        if (len == cap) {
            int64_t *tmp = (int64_t *)realloc(items, cap * 2 * sizeof(int64_t));
            if (!tmp) { free(match); break; }
            items = tmp; cap *= 2;
        }
        items[len++] = (int64_t)(intptr_t)match;
    }
    tkre_free(&re);
    int64_t h = tk_arr_alloc((int64_t)len, (int64_t)len);
    if (!h) { free(items); return 0; }
    memcpy((int64_t *)(intptr_t)h, items, len * sizeof(int64_t));
    free(items);
    return h;
}

/* str.matches(s, pattern) — 1 if the pattern matches the WHOLE of s.
 * 137.9: the whole-string rule is applied by the matcher, not by pasting the
 * pattern into "^(" ... ")$" -- that rewrite let an unbalanced pattern such as
 * `a)(b` compile as `^(a)(b)$` and hid the user's pattern from the error. */
int64_t tk_str_matches_w(int64_t s, int64_t pattern) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    return regex_find1("matches", str, pat, 1);
}

/* str.replacere(s, pattern, replacement) — replace every non-overlapping
 * match with `replacement`, taken literally. */
int64_t tk_str_replacere_w(int64_t s, int64_t pattern, int64_t replacement) {
    const char *str = (const char *)(intptr_t)s;
    const char *pat = (const char *)(intptr_t)pattern;
    const char *rep = (const char *)(intptr_t)replacement;
    if (!str || !pat) return s;
    if (!rep) rep = "";
    TkRe re;
    regex_compile_or_trap("replacere", pat, &re);
    size_t rlen = strlen(rep), slen = strlen(str), opos = 0, done = 0;
    size_t cap = slen + 64;
    char *out = (char *)malloc(cap);
    if (!out) { tkre_free(&re); return s; }
    RegexIter it; regex_iter_init(&it, &re, str);
    for (;;) {
        int more = regex_next("replacere", &it, pat);
        size_t upto = more ? it.ms : slen;          /* copy s[done..upto) */
        size_t need = opos + (upto - done) + (more ? rlen : 0) + 1;
        if (need > cap) {
            while (need > cap) cap *= 2;
            char *tmp = (char *)realloc(out, cap);
            if (!tmp) { free(out); tkre_free(&re); return s; }
            out = tmp;
        }
        memcpy(out + opos, str + done, upto - done); opos += upto - done;
        if (!more) break;
        memcpy(out + opos, rep, rlen); opos += rlen;
        done = it.me;
    }
    out[opos] = '\0';
    tkre_free(&re);
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

/* ── StrBuf builder API ─────────────────────────────────────────────────── */

/* StrBuf is a simple growable buffer stored as a heap struct:
 *   struct { char *data; size_t len; size_t cap; }
 * We pack the pointer to this struct into an i64. */
typedef struct { char *data; size_t len; size_t cap; } TkStrBuilder;

/* str.buf() — create a new string builder */
int64_t tk_str_buf_w(void) {
    TkStrBuilder *buf = (TkStrBuilder *)malloc(sizeof(TkStrBuilder));
    if (!buf) return 0;
    buf->cap = 64;
    buf->data = (char *)malloc(buf->cap);
    if (!buf->data) { free(buf); return 0; }
    buf->len = 0;
    buf->data[0] = '\0';
    return (int64_t)(intptr_t)buf;
}

/* str.add(buf, s) — append a string to the builder */
int64_t tk_str_add_w(int64_t b, int64_t s) {
    TkStrBuilder *buf = (TkStrBuilder *)(intptr_t)b;
    if (!buf) return 0;
    const char *str = s ? (const char *)(intptr_t)s : "";
    size_t slen = strlen(str);
    if (buf->len + slen + 1 > buf->cap) {
        size_t newcap = (buf->len + slen + 1) * 2;
        char *tmp = (char *)realloc(buf->data, newcap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = newcap;
    }
    memcpy(buf->data + buf->len, str, slen);
    buf->len += slen;
    buf->data[buf->len] = '\0';
    return 0;
}

/* str.addbyte(buf, byte) — append a single byte to the builder */
int64_t tk_str_addbyte_w(int64_t b, int64_t ch) {
    TkStrBuilder *buf = (TkStrBuilder *)(intptr_t)b;
    if (!buf) return 0;
    if (buf->len + 2 > buf->cap) {
        size_t newcap = buf->cap * 2;
        char *tmp = (char *)realloc(buf->data, newcap);
        if (!tmp) return 0;
        buf->data = tmp;
        buf->cap = newcap;
    }
    buf->data[buf->len++] = (char)ch;
    buf->data[buf->len] = '\0';
    return 0;
}

/* str.done(buf) — finalize and return the built string */
int64_t tk_str_done_w(int64_t b) {
    TkStrBuilder *buf = (TkStrBuilder *)(intptr_t)b;
    if (!buf) return (int64_t)(intptr_t)"";
    char *result = buf->data;
    free(buf);
    return (int64_t)(intptr_t)result;
}

/* ── .tki name-mismatch aliases (underscore variants) ───────────────────── */

/* str.from_float -> tk_str_from_float_w (alias for tk_str_fromfloat_w) */
int64_t tk_str_from_float_w(int64_t f) { return tk_str_fromfloat_w(f); }

/* str.to_float -> tk_str_to_float_w (alias for tk_str_tofloat_w) */
int64_t tk_str_to_float_w(int64_t s) { return tk_str_tofloat_w(s); }

/* str.from_bytes -> tk_str_from_bytes_w (alias for tk_str_frombytes_w) */
int64_t tk_str_from_bytes_w(int64_t b) { return tk_str_frombytes_w(b); }

/* ── str.charcode / str.charAt / str.length aliases ─────────────────────── */

/* str.charcode(s) — get Unicode code point (byte value) of first char, or
 * str.charcode(s, i) compiled as single-arg by the compiler passing char str.
 * The compiler emits: tk_str_charcode_w(s) where s is a single-char string. */
/* 114.6: str.charcode(s; i) returns the byte code at index i. The wrapper
 * previously ignored i and always returned byte 0. Out-of-range → 0. */
int64_t tk_str_charcode_w(int64_t s, int64_t i) {
    const char *p = s ? (const char *)(intptr_t)s : "";
    if (i < 0) return 0;
    for (int64_t k = 0; k < i; k++) { if (!p[k]) return 0; }
    return (int64_t)(unsigned char)p[i];
}

/* 114.6: str ordering comparators — strcmp-based, returning bool. (str.gt etc.
 * had no `_w` wrapper, so `s.gt(...)` failed to link.) */
int64_t tk_str_gt_w(int64_t a, int64_t b) {
    return strcmp((const char *)(intptr_t)(a ? a : (int64_t)(intptr_t)""),
                  (const char *)(intptr_t)(b ? b : (int64_t)(intptr_t)"")) > 0 ? 1 : 0;
}
int64_t tk_str_lt_w(int64_t a, int64_t b) {
    return strcmp((const char *)(intptr_t)(a ? a : (int64_t)(intptr_t)""),
                  (const char *)(intptr_t)(b ? b : (int64_t)(intptr_t)"")) < 0 ? 1 : 0;
}
int64_t tk_str_ge_w(int64_t a, int64_t b) {
    return strcmp((const char *)(intptr_t)(a ? a : (int64_t)(intptr_t)""),
                  (const char *)(intptr_t)(b ? b : (int64_t)(intptr_t)"")) >= 0 ? 1 : 0;
}
int64_t tk_str_le_w(int64_t a, int64_t b) {
    return strcmp((const char *)(intptr_t)(a ? a : (int64_t)(intptr_t)""),
                  (const char *)(intptr_t)(b ? b : (int64_t)(intptr_t)"")) <= 0 ? 1 : 0;
}

/* str.charAt — camelCase alias for tk_str_charat_w */
int64_t tk_str_charAt_w(int64_t s, int64_t i) {
    return tk_str_charat_w(s, i);
}

/* str.length — alias for tk_str_len_w */
int64_t tk_str_length_w(int64_t s) {
    return tk_str_len_w(s);
}

/* ── std.float glue (no separate file; linked via str_glue) ─────────────── */

/* float.parse(s) — parse string to f64 */
int64_t tk_float_parse_w(int64_t s) {
    return tk_str_tofloat_w(s);
}

/* ── std.fmt glue (no separate file; linked via str_glue) ───────────────── */

/* fmt.f64(value, decimals) — format f64 with N decimal places, returns string */
int64_t tk_fmt_f64_w(int64_t val_bits, int64_t decimals) {
    double d = i64_to_f64(val_bits);
    int dec = (int)decimals;
    if (dec < 0) dec = 0;
    if (dec > 20) dec = 20;
    char buf[64];
    snprintf(buf, sizeof(buf), "%.*f", dec, d);
    char *out = (char *)malloc(strlen(buf) + 1);
    if (!out) return (int64_t)(intptr_t)"0";
    strcpy(out, buf);
    return (int64_t)(intptr_t)out;
}

/* --- str.find(s, sub) — alias for indexof --- */
int64_t tk_str_find_w(int64_t s, int64_t sub) {
    return tk_str_indexof_w(s, sub);
}

/* --- str.chars(s) — split string into array of single characters --- */
int64_t tk_str_chars_w(int64_t s) {
    if (!s) return 0;
    const char *str = (const char *)(intptr_t)s;
    size_t len = strlen(str);
    /* Allocate array: header + one i64 per character */
    int64_t h = tk_arr_alloc((int64_t)len, (int64_t)len);
    if (!h) return 0;
    int64_t *arr = (int64_t *)(intptr_t)h;
    for (size_t i = 0; i < len; i++) {
        char *ch = (char *)malloc(2);
        if (!ch) { arr[i] = (int64_t)(intptr_t)""; continue; }
        ch[0] = str[i];
        ch[1] = '\0';
        arr[i] = (int64_t)(intptr_t)ch;
    }
    return h;
}

/* --- str.sub(s, old, new) — alias for replace --- */
int64_t tk_str_sub_w(int64_t s, int64_t old_val, int64_t new_val) {
    return tk_str_replace_w(s, old_val, new_val);
}

/* --- fmt.sprintf(fmt, val) — format a value into a string --- */
int64_t tk_fmt_sprintf_w(int64_t fmt, int64_t val) {
    if (!fmt) return (int64_t)(intptr_t)"";
    char buf[1024];
    snprintf(buf, sizeof(buf), (const char *)(intptr_t)fmt, (const char *)(intptr_t)val);
    char *out = (char *)malloc(strlen(buf) + 1);
    if (!out) return (int64_t)(intptr_t)"";
    strcpy(out, buf);
    return (int64_t)(intptr_t)out;
}

/* --- str.equals(a, b) — string equality --- */
int64_t tk_str_equals_w(int64_t a, int64_t b) {
    if (!a && !b) return 1;
    if (!a || !b) return 0;
    return strcmp((const char *)(intptr_t)a, (const char *)(intptr_t)b) == 0 ? 1 : 0;
}

/* ─────────────────────────────────────────────────────────────────────────
 * Epic 111 / ADR-0004 — string-building canonical patterns
 *
 * The builder API already exists below as tk_str_buf_w / tk_str_add_w /
 * tk_str_done_w. ADR-0004 names the canonical surface s.builder() /
 * b.add(part) / b.build(); name aliases below redirect to the existing
 * implementation so we don't ship duplicate code.
 *
 * Variadic interpolate: takes an array of $str parts and joins with no
 * separator. Used by interpolation lowering (story 111.5).
 * ──────────────────────────────────────────────────────────────────────── */
int64_t tk_str_interpolate_w(int64_t arr) {
    static const char empty[] = "";
    extern int64_t tk_str_join_w(int64_t, int64_t);
    /* (sep, arr) order — see tk_str_join_w (114.6) */
    return tk_str_join_w((int64_t)(intptr_t)empty, arr);
}
