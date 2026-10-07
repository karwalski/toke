/* SPDX-License-Identifier: MIT */
/*
 * str.c — Implementation of the std.str standard library module.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 1.3.1  Branch: feature/stdlib-str
 */

#include "str.h"
#include "tk_strhdr.h"   /* 127.170: the sealed string heap (state at the end of this file) */
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

uint64_t str_len(const char *s)
{
    if (!s) return 0;
    return (uint64_t)tk_strlen(s);
}

const char *str_concat(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    size_t la = tk_strlen(a), lb = tk_strlen(b);
    char *out = tks_alloc(la + lb);       /* 127.170: sealed below */
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return tks_seal(out, la + lb);
}

StrSliceResult str_slice(const char *s, uint64_t start, uint64_t end)
{
    StrSliceResult r = {NULL, 0, NULL};
    if (!s) { r.is_err = 1; r.err_msg = "null string"; return r; }
    uint64_t slen = (uint64_t)tk_strlen(s);
    if (start > slen || end > slen || start > end) {
        r.is_err = 1;
        r.err_msg = "slice index out of range";
        return r;
    }
    uint64_t n = end - start;
    char *out = tks_dup(s + start, (size_t)n);   /* 127.170: sealed */
    if (!out) { r.is_err = 1; r.err_msg = "allocation failed"; return r; }
    r.ok = out;
    return r;
}

const char *str_from_int(int64_t n)
{
    /* "-9223372036854775808" is 20 chars + NUL */
    char *buf = malloc(24);
    if (!buf) return NULL;
    snprintf(buf, 24, "%" PRId64, n);
    return buf;
}

const char *str_from_float(double n)
{
    char *buf = malloc(64);
    if (!buf) return NULL;
    snprintf(buf, 64, "%g", n);
    return buf;
}

IntParseResult str_to_int(const char *s)
{
    IntParseResult r = {0, 0, NULL};
    if (!s || *s == '\0') { r.is_err = 1; r.err_msg = "empty string"; return r; }
    char *end;
    errno = 0;
    long long v = strtoll(s, &end, 10);
    if (errno != 0 || *end != '\0') {
        r.is_err = 1; r.err_msg = "invalid integer"; return r;
    }
    r.ok = (int64_t)v;
    return r;
}

FloatParseResult str_to_float(const char *s)
{
    FloatParseResult r = {0.0, 0, NULL};
    if (!s || *s == '\0') { r.is_err = 1; r.err_msg = "empty string"; return r; }
    char *end;
    errno = 0;
    double v = strtod(s, &end);
    if (errno != 0 || *end != '\0') {
        r.is_err = 1; r.err_msg = "invalid float"; return r;
    }
    r.ok = v;
    return r;
}

int str_contains(const char *s, const char *sub)
{
    if (!s || !sub) return 0;
    return strstr(s, sub) != NULL ? 1 : 0;
}

StrArray str_split(const char *s, const char *sep)
{
    StrArray result = {NULL, 0};
    if (!s || !sep || *sep == '\0') return result;

    size_t seplen = tk_strlen(sep);
    size_t slen = tk_strlen(s);
    /* count occurrences to pre-size the array */
    size_t count = 1;
    const char *p = s;
    while ((p = strstr(p, sep)) != NULL) { count++; p += seplen; }

    result.data = malloc(count * sizeof(const char *));
    if (!result.data) return result;

    p = s;
    size_t i = 0;
    size_t n = 0;
    const char *next;
    while ((next = strstr(p, sep)) != NULL && i < count - 1) {
        n = (size_t)(next - p);
        char *piece = tks_dup(p, n);          /* 127.170: sealed */
        if (!piece) break;
        result.data[i++] = piece;
        p = next + seplen;
    }
    /* last segment */
    n = slen - (size_t)(p - s);
    char *tail = tks_dup(p, n);
    if (tail) result.data[i++] = tail;
    result.len = i;
    return result;
}

const char *str_trim(const char *s)
{
    if (!s) return NULL;
    while (*s && isspace((unsigned char)*s)) s++;
    size_t len = strlen(s);
    while (len > 0 && isspace((unsigned char)s[len - 1])) len--;
    char *out = malloc(len + 1);
    if (!out) return NULL;
    memcpy(out, s, len);
    out[len] = '\0';
    return out;
}

const char *str_upper(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++)
        out[i] = (char)toupper((unsigned char)s[i]);
    out[len] = '\0';
    return out;
}

const char *str_lower(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++)
        out[i] = (char)tolower((unsigned char)s[i]);
    out[len] = '\0';
    return out;
}

ByteArray str_bytes(const char *s)
{
    ByteArray r = {NULL, 0};
    if (!s) return r;
    r.data = (const uint8_t *)s;
    r.len  = (uint64_t)strlen(s);
    return r;
}

/* Validate UTF-8 encoding per RFC 3629. */
static int is_valid_utf8(const uint8_t *b, uint64_t len)
{
    uint64_t i = 0;
    while (i < len) {
        uint8_t c = b[i];
        int seq;
        if      (c < 0x80)                    seq = 1;
        else if ((c & 0xE0) == 0xC0)          seq = 2;
        else if ((c & 0xF0) == 0xE0)          seq = 3;
        else if ((c & 0xF8) == 0xF0)          seq = 4;
        else return 0;
        if (i + (uint64_t)seq > len) return 0;
        for (int j = 1; j < seq; j++)
            if ((b[i + (uint64_t)j] & 0xC0) != 0x80) return 0;
        i += (uint64_t)seq;
    }
    return 1;
}

StrEncResult str_from_bytes(ByteArray b)
{
    StrEncResult r = {NULL, 0, NULL};
    if (!b.data && b.len == 0) { r.ok = ""; return r; }
    if (!b.data) { r.is_err = 1; r.err_msg = "null byte array"; return r; }
    if (!is_valid_utf8(b.data, b.len)) {
        r.is_err = 1; r.err_msg = "invalid UTF-8 encoding"; return r;
    }
    char *out = malloc(b.len + 1);
    if (!out) { r.is_err = 1; r.err_msg = "allocation failed"; return r; }
    memcpy(out, b.data, b.len);
    out[b.len] = '\0';
    r.ok = out;
    return r;
}

/* --- Story 78.4.1: printf-style single-argument format ------------------- */

const char *str_format(const char *fmt, int64_t arg)
{
    if (!fmt) return NULL;

    /*
     * Single-pass approach using snprintf with the extracted format specifier.
     * We support one substitution per call.  Format specifiers may include
     * width, precision, and flags (e.g. "%.2f", "%10d", "%-8s").
     */
    char tmp[256];
    char spec_buf[64];  /* holds the extracted format specifier e.g. "%.2f" */

    /* Find the first real format specifier (skip %%) */
    const char *p = fmt;
    const char *spec_start = NULL;
    const char *spec_end = NULL;
    char conversion = 0;

    while (*p) {
        if (*p == '%') {
            if (*(p + 1) == '%') { p += 2; continue; }
            /* Start of a format specifier */
            spec_start = p;
            p++;
            /* Skip flags: -, +, 0, space, # */
            while (*p == '-' || *p == '+' || *p == '0' || *p == ' ' || *p == '#') p++;
            /* Skip width */
            while (*p >= '0' && *p <= '9') p++;
            /* Skip precision */
            if (*p == '.') { p++; while (*p >= '0' && *p <= '9') p++; }
            /* Conversion character */
            if (*p) { conversion = *p; spec_end = p + 1; }
            break;
        } else {
            p++;
        }
    }

    if (!spec_start || !spec_end || !conversion) {
        /* No format specifier found, return fmt as-is (with %% -> %) */
        size_t len = strlen(fmt);
        char *out = malloc(len + 1);
        if (!out) return NULL;
        char *w = out;
        for (const char *r = fmt; *r; ) {
            if (*r == '%' && *(r+1) == '%') { *w++ = '%'; r += 2; }
            else *w++ = *r++;
        }
        *w = '\0';
        return out;
    }

    /* Extract the format specifier into spec_buf */
    size_t spec_len = (size_t)(spec_end - spec_start);
    if (spec_len >= sizeof(spec_buf)) spec_len = sizeof(spec_buf) - 1;
    memcpy(spec_buf, spec_start, spec_len);
    spec_buf[spec_len] = '\0';

    /* Format the argument according to the conversion character */
    int tmp_len = 0;
    if (conversion == 's') {
        const char *s = (const char *)(intptr_t)arg;
        if (!s) s = "(null)";
        tmp_len = snprintf(tmp, sizeof tmp, spec_buf, s);
    } else if (conversion == 'd' || conversion == 'i') {
        /* Replace the conversion with PRId64-compatible one */
        spec_buf[spec_len - 1] = '\0';  /* remove 'd' or 'i' */
        char i64_spec[80];
        snprintf(i64_spec, sizeof i64_spec, "%s" PRId64, spec_buf);
        tmp_len = snprintf(tmp, sizeof tmp, i64_spec, arg);
    } else if (conversion == 'u') {
        spec_buf[spec_len - 1] = '\0';
        char u64_spec[80];
        snprintf(u64_spec, sizeof u64_spec, "%s" PRIu64, spec_buf);
        tmp_len = snprintf(tmp, sizeof tmp, u64_spec, (uint64_t)arg);
    } else if (conversion == 'f' || conversion == 'e' || conversion == 'g' ||
               conversion == 'F' || conversion == 'E' || conversion == 'G') {
        double d;
        memcpy(&d, &arg, sizeof(d));
        tmp_len = snprintf(tmp, sizeof tmp, spec_buf, d);
    } else if (conversion == 'x' || conversion == 'X') {
        spec_buf[spec_len - 1] = '\0';
        char hex_spec[80];
        snprintf(hex_spec, sizeof hex_spec, "%s%s",
                 spec_buf, conversion == 'x' ? PRIx64 : PRIX64);
        tmp_len = snprintf(tmp, sizeof tmp, hex_spec, (uint64_t)arg);
    } else if (conversion == 'c') {
        tmp_len = snprintf(tmp, sizeof tmp, "%c", (char)arg);
    } else {
        /* Unknown conversion — just use it raw */
        tmp_len = snprintf(tmp, sizeof tmp, "%s", spec_buf);
    }

    if (tmp_len < 0) tmp_len = 0;
    if ((size_t)tmp_len >= sizeof(tmp)) tmp_len = (int)(sizeof(tmp) - 1);

    /* Build the output: prefix + formatted arg + suffix */
    size_t prefix_len = (size_t)(spec_start - fmt);
    size_t suffix_len = strlen(spec_end);
    /* Handle %% in prefix and suffix */
    size_t out_len = prefix_len + (size_t)tmp_len + suffix_len + 1;
    char *out = malloc(out_len);
    if (!out) return NULL;

    /* Copy prefix, converting %% to % */
    char *w = out;
    for (const char *r = fmt; r < spec_start; ) {
        if (*r == '%' && *(r+1) == '%') { *w++ = '%'; r += 2; }
        else *w++ = *r++;
    }
    /* Copy formatted value */
    memcpy(w, tmp, (size_t)tmp_len);
    w += tmp_len;
    /* Copy suffix, converting %% to % */
    for (const char *r = spec_end; *r; ) {
        if (*r == '%' && *(r+1) == '%') { *w++ = '%'; r += 2; }
        else *w++ = *r++;
    }
    *w = '\0';
    return out;
}

/* --- Story 28.1.1: search and transform ---------------------------------- */

int64_t str_index(const char *s, const char *sub)
{
    if (!s || !sub) return -1;
    const char *p = strstr(s, sub);
    if (!p) return -1;
    return (int64_t)(p - s);
}

const char *str_replace(const char *s, const char *old, const char *new_val)
{
    if (!s || !old || !new_val) return NULL;
    size_t oldlen = tk_strlen(old);
    size_t newlen = tk_strlen(new_val);
    size_t slen = tk_strlen(s);
    if (oldlen == 0) {
        /* nothing to replace — return copy */
        return tks_dup(s, slen);              /* 127.170: sealed */
    }
    /* count occurrences */
    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, old)) != NULL) { count++; p += oldlen; }
    size_t outlen = slen - count * oldlen + count * newlen;
    char *out = tks_alloc(outlen);            /* 127.170: sealed below */
    if (!out) return NULL;
    char *w = out;
    p = s;
    const char *found;
    while ((found = strstr(p, old)) != NULL) {
        size_t before = (size_t)(found - p);
        memcpy(w, p, before);
        w += before;
        memcpy(w, new_val, newlen);
        w += newlen;
        p = found + oldlen;
    }
    size_t tail = slen - (size_t)(p - s);
    memcpy(w, p, tail + 1); /* include NUL */
    return tks_seal(out, outlen);
}

const char *str_replace_first(const char *s, const char *old, const char *new_val)
{
    if (!s || !old || !new_val) return NULL;
    size_t oldlen = tk_strlen(old);
    size_t slen = tk_strlen(s);
    if (oldlen == 0) return tks_dup(s, slen);            /* 127.170: sealed */
    const char *found = strstr(s, old);
    if (!found) return tks_dup(s, slen);
    size_t newlen  = tk_strlen(new_val);
    size_t before  = (size_t)(found - s);
    size_t after   = slen - before - oldlen;
    size_t outlen  = before + newlen + after;
    char *out = tks_alloc(outlen);
    if (!out) return NULL;
    memcpy(out, s, before);
    memcpy(out + before, new_val, newlen);
    memcpy(out + before + newlen, found + oldlen, after + 1);
    return tks_seal(out, outlen);
}

const char *str_join(const char *sep, StrArray parts)
{
    if (!sep) sep = "";
    if (parts.len == 0 || !parts.data) return tks_dup("", 0);   /* 127.170 */
    size_t seplen = tk_strlen(sep);
    size_t total  = 0;
    uint64_t i;
    for (i = 0; i < parts.len; i++) {
        if (parts.data[i]) total += tk_strlen(parts.data[i]);
        if (i + 1 < parts.len) total += seplen;
    }
    char *out = tks_alloc(total);             /* 127.170: sealed below */
    if (!out) return NULL;
    char *w = out;
    for (i = 0; i < parts.len; i++) {
        if (parts.data[i]) {
            size_t plen = tk_strlen(parts.data[i]);
            memcpy(w, parts.data[i], plen);
            w += plen;
        }
        if (i + 1 < parts.len) {
            memcpy(w, sep, seplen);
            w += seplen;
        }
    }
    *w = '\0';
    return tks_seal(out, total);
}

const char *str_repeat(const char *s, uint64_t n)
{
    if (!s) return NULL;
    if (n == 0) {
        char *empty = malloc(1);
        if (!empty) return NULL;
        empty[0] = '\0';
        return empty;
    }
    size_t slen  = strlen(s);
    size_t total = slen * (size_t)n;
    char *out = malloc(total + 1);
    if (!out) return NULL;
    for (uint64_t i = 0; i < n; i++)
        memcpy(out + i * slen, s, slen);
    out[total] = '\0';
    return out;
}

/* --- Story 28.1.2: prefix/suffix and line operations --------------------- */

int str_starts_with(const char *s, const char *prefix)
{
    if (!s || !prefix) return 0;
    size_t plen = strlen(prefix);
    return strncmp(s, prefix, plen) == 0 ? 1 : 0;
}

int str_ends_with(const char *s, const char *suffix)
{
    if (!s || !suffix) return 0;
    size_t slen    = tk_strlen(s);
    size_t suflen  = tk_strlen(suffix);
    if (suflen > slen) return 0;
    return strcmp(s + slen - suflen, suffix) == 0 ? 1 : 0;
}

StrArray str_split_lines(const char *s)
{
    StrArray result = {NULL, 0};
    if (!s) return result;

    /* count lines */
    size_t count = 1;
    const char *p = s;
    while (*p) {
        if (*p == '\n') count++;
        p++;
    }

    result.data = malloc(count * sizeof(const char *));
    if (!result.data) return result;

    size_t i = 0;
    p = s;
    const char *line_start = s;
    while (*p) {
        if (*p == '\n') {
            size_t llen = (size_t)(p - line_start);
            /* strip trailing \r if present (CRLF) */
            if (llen > 0 && line_start[llen - 1] == '\r') llen--;
            char *piece = malloc(llen + 1);
            if (!piece) break;
            memcpy(piece, line_start, llen);
            piece[llen] = '\0';
            result.data[i++] = piece;
            line_start = p + 1;
        }
        p++;
    }
    /* last (or only) line, possibly empty */
    {
        size_t llen = (size_t)(p - line_start);
        if (llen > 0 && line_start[llen - 1] == '\r') llen--;
        char *piece = malloc(llen + 1);
        if (piece) {
            memcpy(piece, line_start, llen);
            piece[llen] = '\0';
            result.data[i++] = piece;
        }
    }
    result.len = i;
    return result;
}

uint64_t str_count(const char *s, const char *sub)
{
    if (!s || !sub || *sub == '\0') return 0;
    uint64_t count  = 0;
    size_t   sublen = strlen(sub);
    const char *p   = s;
    while ((p = strstr(p, sub)) != NULL) {
        count++;
        p += sublen;
    }
    return count;
}

/* --- Story 28.1.3: padding, reverse, and character class tests ----------- */

const char *str_pad_left(const char *s, uint64_t width, char ch)
{
    if (!s) return NULL;
    size_t slen = strlen(s);
    if ((uint64_t)slen >= width) {
        char *copy = malloc(slen + 1);
        if (!copy) return NULL;
        memcpy(copy, s, slen + 1);
        return copy;
    }
    size_t pad = (size_t)(width - (uint64_t)slen);
    char *out = malloc(width + 1);
    if (!out) return NULL;
    memset(out, ch, pad);
    memcpy(out + pad, s, slen);
    out[width] = '\0';
    return out;
}

const char *str_pad_right(const char *s, uint64_t width, char ch)
{
    if (!s) return NULL;
    size_t slen = strlen(s);
    if ((uint64_t)slen >= width) {
        char *copy = malloc(slen + 1);
        if (!copy) return NULL;
        memcpy(copy, s, slen + 1);
        return copy;
    }
    size_t pad = (size_t)(width - (uint64_t)slen);
    char *out = malloc(width + 1);
    if (!out) return NULL;
    memcpy(out, s, slen);
    memset(out + slen, ch, pad);
    out[width] = '\0';
    return out;
}

const char *str_reverse(const char *s)
{
    if (!s) return NULL;
    size_t len = strlen(s);
    char *out = malloc(len + 1);
    if (!out) return NULL;
    for (size_t i = 0; i < len; i++)
        out[i] = s[len - 1 - i];
    out[len] = '\0';
    return out;
}

int str_is_alpha(const char *s)
{
    if (!s || *s == '\0') return 0;
    while (*s) {
        if (!isalpha((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

int str_is_digit(const char *s)
{
    if (!s || *s == '\0') return 0;
    while (*s) {
        if (!isdigit((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

int str_is_alnum(const char *s)
{
    if (!s || *s == '\0') return 0;
    while (*s) {
        if (!isalnum((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

int str_is_space(const char *s)
{
    if (!s || *s == '\0') return 0;
    while (*s) {
        if (!isspace((unsigned char)*s)) return 0;
        s++;
    }
    return 1;
}

/* --- Story 56.8.5: trimprefix, trimsuffix, lastindex, matchbracket -------- */

const char *str_trimprefix(const char *s, const char *prefix)
{
    if (!s) return strdup("");
    if (!prefix || prefix[0] == '\0') return strdup(s);
    size_t plen = strlen(prefix);
    if (strncmp(s, prefix, plen) == 0)
        return strdup(s + plen);
    return strdup(s);
}

const char *str_trimsuffix(const char *s, const char *suffix)
{
    if (!s) return strdup("");
    if (!suffix || suffix[0] == '\0') return strdup(s);
    size_t slen   = tk_strlen(s);
    size_t suflen = tk_strlen(suffix);
    if (slen >= suflen && memcmp(s + slen - suflen, suffix, suflen) == 0) {
        char *r = malloc(slen - suflen + 1);
        if (!r) return strdup(s);
        memcpy(r, s, slen - suflen);
        r[slen - suflen] = '\0';
        return r;
    }
    return strdup(s);
}

int64_t str_lastindex(const char *s, const char *sub)
{
    if (!s || !sub) return -1;
    size_t slen   = strlen(s);
    size_t sublen = strlen(sub);
    if (sublen == 0) return (int64_t)slen;
    if (sublen > slen) return -1;
    for (size_t i = slen - sublen + 1; i-- > 0; ) {
        if (memcmp(s + i, sub, sublen) == 0)
            return (int64_t)i;
    }
    return -1;
}

StrBracketResult str_matchbracket(const char *s)
{
    StrBracketResult r = {NULL, 0, NULL};
    if (!s || s[0] != '[') {
        r.is_err = 1; r.err_msg = "not a bracket segment"; return r;
    }
    size_t slen = strlen(s);
    if (slen < 3 || s[slen - 1] != ']') {
        /* need at least '[', one char of name, ']' */
        r.is_err = 1; r.err_msg = "not a bracket segment"; return r;
    }
    /* extract content between brackets */
    size_t namelen = slen - 2;  /* strip leading '[' and trailing ']' */
    char *name = malloc(namelen + 1);
    if (!name) { r.is_err = 1; r.err_msg = "allocation failed"; return r; }
    memcpy(name, s + 1, namelen);
    name[namelen] = '\0';
    r.ok = name;
    return r;
}

/* --- Story 55.4.6: string builder ---*/

/* --- Story 127.170: the sealed string heap (tk_strhdr.h) -----------------
 *
 * The state behind tk_strhdr.h, in the one translation unit every compiled
 * toke program and the compiler link.  See the header for the layout and the
 * rule; docs/runtime-abi.md §3.1 for the design and the gates.
 */
char    *tks_lo;          /* region base; NULL until reserved */
uint8_t *tks_bits;        /* TKS_RESERVE/128 bytes, one bit per granule */
int      tks_verify;      /* set once, before tks_lo is published */

void tks_verify_fail(const char *p, int64_t hdr)
{
    fprintf(stderr, "TK_STR_VERIFY: header %lld != strlen %zu at %p "
            "(a sealed string was written after tks_seal)\n",
            (long long)hdr, strlen(p), (const void *)p);
    fflush(stderr);
    abort();
}

#if TKS_HEAP
#include <sys/mman.h>
#include <sched.h>
#include <pthread.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define TKS_ASAN 1
#endif
#endif
#if !defined(TKS_ASAN) && defined(__SANITIZE_ADDRESS__)
#define TKS_ASAN 1
#endif
#ifdef TKS_ASAN
#include <sanitizer/asan_interface.h>
#define TKS_REDZONE 16    /* poisoned granule after every string under ASan */
#else
#define TKS_REDZONE 0
#endif

#ifndef MAP_NORESERVE
#define MAP_NORESERVE 0
#endif
#ifndef MAP_ANON
#define MAP_ANON MAP_ANONYMOUS
#endif

static size_t tks_top;        /* bytes handed out (may run past the end) */
static size_t tks_committed;  /* region bytes mprotect'd read/write */
static int    tks_state;      /* 0 unreserved, 2 ready, 3 failed */
static int    tks_lock_word;

static void tks_lock(void)
{
    while (__atomic_exchange_n(&tks_lock_word, 1, __ATOMIC_ACQUIRE))
        sched_yield();
}
static void tks_unlock(void) { __atomic_store_n(&tks_lock_word, 0, __ATOMIC_RELEASE); }

/* Reserve the region and the bitmap, once.  Returns 1 when the heap is
 * usable.  Nothing is committed here: both mappings are PROT_NONE. */
static int tks_reserve(void)
{
    int st = __atomic_load_n(&tks_state, __ATOMIC_ACQUIRE);
    if (st) return st == 2;
    tks_lock();
    st = __atomic_load_n(&tks_state, __ATOMIC_RELAXED);
    if (!st) {
        const char *e = getenv("TK_STR_VERIFY");
        tks_verify = (TK_STR_VERIFY || (e && e[0] == '1')) ? 1 : 0;
        void *r = mmap(NULL, (size_t)TKS_RESERVE, PROT_NONE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        void *b = mmap(NULL, (size_t)(TKS_RESERVE / 128), PROT_NONE,
                       MAP_PRIVATE | MAP_ANON | MAP_NORESERVE, -1, 0);
        if (r == MAP_FAILED || b == MAP_FAILED) {
            if (r != MAP_FAILED) munmap(r, (size_t)TKS_RESERVE);
            if (b != MAP_FAILED) munmap(b, (size_t)(TKS_RESERVE / 128));
            st = 3;
        } else {
            tks_bits = (uint8_t *)b;
            __atomic_store_n(&tks_lo, (char *)r, __ATOMIC_RELEASE);
            st = 2;
            /* The http server forks workers.  A fork while another thread
             * holds the lock (mid-commit) would leave the child spinning
             * forever; hold it across fork so the child's copy is free. */
            (void)pthread_atfork(tks_lock, tks_unlock, tks_unlock);
        }
        __atomic_store_n(&tks_state, st, __ATOMIC_RELEASE);
    }
    tks_unlock();
    return st == 2;
}

/* Make region bytes [0, end) and their bitmap read/write.  Returns 0 when
 * the OS refuses (strict overcommit, RLIMIT_AS): the caller falls back. */
static int tks_commit(size_t end)
{
    int ok = 1;
    tks_lock();
    size_t have = __atomic_load_n(&tks_committed, __ATOMIC_RELAXED);
    while (have < end) {
        if (mprotect(tks_lo + have, TKS_CHUNK, PROT_READ | PROT_WRITE) != 0 ||
            mprotect(tks_bits + have / 128, TKS_CHUNK / 128,
                     PROT_READ | PROT_WRITE) != 0) { ok = 0; break; }
#ifdef TKS_ASAN
        /* Fresh pages are unpoisoned; each allocation poisons its own tail. */
#endif
        have += TKS_CHUNK;
        __atomic_store_n(&tks_committed, have, __ATOMIC_RELEASE);
    }
    tks_unlock();
    return ok;
}

char *tks_alloc(size_t n)
{
    if (n < (size_t)TKS_RESERVE && tks_reserve()) {
        size_t need = (8 + n + 1 + TKS_REDZONE + 15) & ~(size_t)15;
        size_t off = __atomic_fetch_add(&tks_top, need, __ATOMIC_RELAXED);
        if (off + need <= (size_t)TKS_RESERVE &&
            (off + need <= __atomic_load_n(&tks_committed, __ATOMIC_ACQUIRE) ||
             tks_commit(off + need))) {
            char *p = tks_lo + off + 8;
#ifdef TKS_ASAN
            /* Everything past the NUL to the end of the block is off limits,
             * so an over-read of a sealed string is reported as it would be
             * for a malloc'd one. */
            ASAN_POISON_MEMORY_REGION(p + n + 1, (size_t)(tks_lo + off + need - (p + n + 1)));
#endif
            return p;
        }
    }
    return (char *)malloc(n + 1);
}

char *tks_seal(char *p, size_t n)
{
    char *lo = __atomic_load_n(&tks_lo, __ATOMIC_ACQUIRE);
    uint64_t o = (uint64_t)((uintptr_t)p - (uintptr_t)lo);
    if (p && lo && o < TKS_RESERVE && (o & 15) == 8) {
        int64_t len = (int64_t)n;
        memcpy(p - 8, &len, sizeof len);
        uint64_t g = o >> 4;
        __atomic_fetch_or(&tks_bits[g >> 3], (uint8_t)(1u << (g & 7)), __ATOMIC_RELEASE);
    }
    return p;
}

#else /* !TKS_HEAP: every string is a malloc'd, unsealed C string */

char *tks_alloc(size_t n) { return (char *)malloc(n + 1); }
char *tks_seal(char *p, size_t n) { (void)n; return p; }

#endif /* TKS_HEAP */

char *tks_dup(const char *s, size_t n)
{
    char *p = tks_alloc(n);
    if (!p) return NULL;
    memcpy(p, s, n);
    p[n] = '\0';
    return tks_seal(p, n);
}

char *tks_strdup(const char *s)
{
    return tks_dup(s, tk_strlen(s));
}
