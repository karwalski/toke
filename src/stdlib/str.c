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
