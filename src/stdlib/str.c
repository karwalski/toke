/*
 * str.c — Implementation of the std.str standard library module.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 1.3.1  Branch: feature/stdlib-str
 */

#include "str.h"
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <errno.h>
#include <inttypes.h>

uint64_t str_len(const char *s)
{
    if (!s) return 0;
    return (uint64_t)strlen(s);
}

const char *str_concat(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";
    size_t la = strlen(a), lb = strlen(b);
    char *out = malloc(la + lb + 1);
    if (!out) return NULL;
    memcpy(out, a, la);
    memcpy(out + la, b, lb);
    out[la + lb] = '\0';
    return out;
}

StrSliceResult str_slice(const char *s, uint64_t start, uint64_t end)
{
    StrSliceResult r = {NULL, 0, NULL};
    if (!s) { r.is_err = 1; r.err_msg = "null string"; return r; }
    uint64_t slen = (uint64_t)strlen(s);
    if (start > slen || end > slen || start > end) {
        r.is_err = 1;
        r.err_msg = "slice index out of range";
        return r;
    }
    uint64_t n = end - start;
    char *out = malloc(n + 1);
    if (!out) { r.is_err = 1; r.err_msg = "allocation failed"; return r; }
    memcpy(out, s + start, n);
    out[n] = '\0';
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

    size_t seplen = strlen(sep);
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
        char *piece = malloc(n + 1);
        if (!piece) break;
        memcpy(piece, p, n);
        piece[n] = '\0';
        result.data[i++] = piece;
        p = next + seplen;
    }
    /* last segment */
    n = strlen(p);
    char *tail = malloc(n + 1);
    if (tail) { memcpy(tail, p, n); tail[n] = '\0'; result.data[i++] = tail; }
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

/* --- Story 28.1.1: search and transform ---------------------------------- */

int64_t str_index(const char *s, const char *sub)
{
    if (!s || !sub) return -1;
    const char *p = strstr(s, sub);
    if (!p) return -1;
    return (int64_t)(p - s);
}

int64_t str_rindex(const char *s, const char *sub)
{
    if (!s || !sub) return -1;
    size_t slen   = strlen(s);
    size_t sublen = strlen(sub);
    if (sublen == 0) return (int64_t)slen; /* convention: empty sub at end */
    if (sublen > slen) return -1;
    /* scan from right */
    const char *last = NULL;
    const char *p    = s;
    while ((p = strstr(p, sub)) != NULL) {
        last = p;
        p++;
    }
    return last ? (int64_t)(last - s) : -1;
}

const char *str_replace(const char *s, const char *old, const char *new_val)
{
    if (!s || !old || !new_val) return NULL;
    size_t oldlen = strlen(old);
    size_t newlen = strlen(new_val);
    if (oldlen == 0) {
        /* nothing to replace — return copy */
        size_t slen = strlen(s);
        char *copy = malloc(slen + 1);
        if (!copy) return NULL;
        memcpy(copy, s, slen + 1);
        return copy;
    }
    /* count occurrences */
    size_t count = 0;
    const char *p = s;
    while ((p = strstr(p, old)) != NULL) { count++; p += oldlen; }
    size_t slen = strlen(s);
    size_t outsz = slen + count * (newlen - oldlen) + 1; /* may be smaller */
    /* newlen >= oldlen path: outsz is fine; newlen < oldlen: shrinks, still fine */
    char *out = malloc(outsz);
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
    size_t tail = strlen(p);
    memcpy(w, p, tail + 1); /* include NUL */
    return out;
}

const char *str_replace_first(const char *s, const char *old, const char *new_val)
{
    if (!s || !old || !new_val) return NULL;
    size_t oldlen = strlen(old);
    if (oldlen == 0) {
        size_t slen = strlen(s);
        char *copy = malloc(slen + 1);
        if (!copy) return NULL;
        memcpy(copy, s, slen + 1);
        return copy;
    }
    const char *found = strstr(s, old);
    if (!found) {
        size_t slen = strlen(s);
        char *copy = malloc(slen + 1);
        if (!copy) return NULL;
        memcpy(copy, s, slen + 1);
        return copy;
    }
    size_t newlen  = strlen(new_val);
    size_t before  = (size_t)(found - s);
    size_t after   = strlen(found + oldlen);
    size_t outsz   = before + newlen + after + 1;
    char *out = malloc(outsz);
    if (!out) return NULL;
    memcpy(out, s, before);
    memcpy(out + before, new_val, newlen);
    memcpy(out + before + newlen, found + oldlen, after + 1);
    return out;
}

const char *str_join(const char *sep, StrArray parts)
{
    if (!sep) sep = "";
    if (parts.len == 0 || !parts.data) {
        char *empty = malloc(1);
        if (!empty) return NULL;
        empty[0] = '\0';
        return empty;
    }
    size_t seplen = strlen(sep);
    size_t total  = 0;
    uint64_t i;
    for (i = 0; i < parts.len; i++) {
        if (parts.data[i]) total += strlen(parts.data[i]);
        if (i + 1 < parts.len) total += seplen;
    }
    char *out = malloc(total + 1);
    if (!out) return NULL;
    char *w = out;
    for (i = 0; i < parts.len; i++) {
        if (parts.data[i]) {
            size_t plen = strlen(parts.data[i]);
            memcpy(w, parts.data[i], plen);
            w += plen;
        }
        if (i + 1 < parts.len) {
            memcpy(w, sep, seplen);
            w += seplen;
        }
    }
    *w = '\0';
    return out;
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
    size_t slen    = strlen(s);
    size_t suflen  = strlen(suffix);
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
