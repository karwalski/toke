/*
 * fmt.c — Implementation of the std.fmt standard library module.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. All returned strings are caller-owned (runtime-abi §3).
 *
 * Story: 131.30  value formatting for print (bool / arrays / pad).
 */

#include "fmt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static char *dup_cstr(const char *s)
{
    size_t n = strlen(s);
    char *out = (char *)malloc(n + 1);
    if (!out) return NULL;
    memcpy(out, s, n + 1);
    return out;
}

char *fmt_bool(int b)
{
    return dup_cstr(b ? "true" : "false");
}

char *fmt_join_i64(const int64_t *xs, int64_t n, const char *sep)
{
    if (!sep) sep = "";
    if (n <= 0 || !xs) return dup_cstr("");
    size_t seplen = strlen(sep);
    /* worst case per element: "-9223372036854775808" = 20 chars */
    size_t cap = (size_t)n * (20 + seplen) + 1;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t pos = 0;
    for (int64_t i = 0; i < n; i++) {
        if (i) { memcpy(out + pos, sep, seplen); pos += seplen; }
        pos += (size_t)snprintf(out + pos, cap - pos, "%lld", (long long)xs[i]);
    }
    out[pos] = '\0';
    return out;
}

char *fmt_join_str(const char *const *xs, int64_t n, const char *sep)
{
    if (!sep) sep = "";
    if (n <= 0 || !xs) return dup_cstr("");
    size_t seplen = strlen(sep);
    size_t total = 1 + seplen * (size_t)(n - 1);
    for (int64_t i = 0; i < n; i++) total += xs[i] ? strlen(xs[i]) : 0;
    char *out = (char *)malloc(total);
    if (!out) return NULL;
    size_t pos = 0;
    for (int64_t i = 0; i < n; i++) {
        if (i) { memcpy(out + pos, sep, seplen); pos += seplen; }
        if (xs[i]) { size_t l = strlen(xs[i]); memcpy(out + pos, xs[i], l); pos += l; }
    }
    out[pos] = '\0';
    return out;
}

/* Count UTF-8 code points: every byte that is not a continuation byte. */
static int64_t utf8_width(const char *s)
{
    int64_t w = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) w++;
    return w;
}

char *fmt_pad(const char *s, int64_t width, int left)
{
    if (!s) s = "";
    size_t slen = strlen(s);
    int64_t have = utf8_width(s);
    if (width <= have) return dup_cstr(s);
    size_t padn = (size_t)(width - have);
    char *out = (char *)malloc(slen + padn + 1);
    if (!out) return NULL;
    if (left) {
        memset(out, ' ', padn);
        memcpy(out + padn, s, slen);
    } else {
        memcpy(out, s, slen);
        memset(out + slen, ' ', padn);
    }
    out[slen + padn] = '\0';
    return out;
}
