/*
 * tk_gzip_type.h — which response content types gzip leaves alone
 * (story 121.89).
 *
 * Images, audio, video, fonts, wasm and archives are compressed already:
 * gzip spends CPU on them and the result is about as large, sometimes
 * larger.  Until 121.86 http.c never met one whole (a binary body was cut at
 * its first 0x00), so maybe_gzip_body never had to ask; router.c's
 * gzip_skip_type asked, with a shorter list.  This is the one list both use:
 * http.c's maybe_gzip_body (HTTP/1.1, TLS and HTTP/2 all go through it) and
 * router.c's gzip middleware.
 *
 * Skipped: image/ (except image/svg+xml, which is XML text), video/, audio/,
 * font/, application/octet-stream, application/wasm, application/zip,
 * application/gzip (and its alias application/x-gzip).  The match ignores
 * case, leading blanks and parameters ("image/png; q=1").  No content type
 * at all is compressible, as before.
 *
 * Header-only and static, like tk_bind.h, so every stdlib translation unit
 * that includes it links without a new object in any link line.
 */
#ifndef TK_GZIP_TYPE_H
#define TK_GZIP_TYPE_H

#include <stddef.h>
#include <strings.h>

#if defined(__GNUC__) || defined(__clang__)
#define TK_GZIP_TYPE_UNUSED __attribute__((unused))
#else
#define TK_GZIP_TYPE_UNUSED
#endif

/* 1 when ct begins with prefix, ignoring case. */
static TK_GZIP_TYPE_UNUSED int tk_gzip_type_prefix(const char *ct, const char *prefix)
{
    size_t n = 0;
    while (prefix[n]) n++;
    return strncasecmp(ct, prefix, n) == 0;
}

/* 1 when ct is exactly the media type `type`, ignoring case and parameters. */
static TK_GZIP_TYPE_UNUSED int tk_gzip_type_is(const char *ct, const char *type)
{
    size_t n = 0;
    while (type[n]) n++;
    if (strncasecmp(ct, type, n) != 0) return 0;
    char c = ct[n];
    return c == '\0' || c == ';' || c == ' ' || c == '\t';
}

/* 1 when a body of Content-Type ct should be sent without gzip. */
static TK_GZIP_TYPE_UNUSED int tk_gzip_skip_type(const char *ct)
{
    if (!ct) return 0;
    while (*ct == ' ' || *ct == '\t') ct++;
    if (tk_gzip_type_is(ct, "image/svg+xml")) return 0;
    if (tk_gzip_type_prefix(ct, "image/")) return 1;
    if (tk_gzip_type_prefix(ct, "video/")) return 1;
    if (tk_gzip_type_prefix(ct, "audio/")) return 1;
    if (tk_gzip_type_prefix(ct, "font/"))  return 1;
    if (tk_gzip_type_is(ct, "application/octet-stream")) return 1;
    if (tk_gzip_type_is(ct, "application/wasm"))   return 1;
    if (tk_gzip_type_is(ct, "application/zip"))    return 1;
    if (tk_gzip_type_is(ct, "application/gzip"))   return 1;
    if (tk_gzip_type_is(ct, "application/x-gzip")) return 1;
    return 0;
}

#endif /* TK_GZIP_TYPE_H */
