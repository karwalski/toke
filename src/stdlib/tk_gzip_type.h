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
#include <string.h>
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

/* ── Accept-Encoding (stories 121.94, 121.96) ───────────────────────── */

/* 1 when the n bytes at v are an RFC 9110 qvalue (sec. 12.4.2) above zero:
 * "0" [ "." 0*3DIGIT ] / "1" [ "." 0*3("0") ].  A malformed weight is
 * treated as zero -- not acceptable -- so a header the server cannot read
 * never gets a coding the client may not handle. */
static TK_GZIP_TYPE_UNUSED int tk_gzip_qvalue_positive(const char *v, size_t n)
{
    if (n == 0 || (v[0] != '0' && v[0] != '1')) return 0;
    int pos = v[0] == '1';
    if (n == 1) return pos;
    if (v[1] != '.' || n > 5) return 0;
    for (size_t i = 2; i < n; i++) {
        if (v[i] < '0' || v[i] > '9') return 0;
        if (v[i] != '0') { if (v[0] == '1') return 0; pos = 1; }
    }
    return pos;
}

static TK_GZIP_TYPE_UNUSED int tk_gzip_ows(char c) { return c == ' ' || c == '\t'; }

/* 1 when the n bytes at name are the content coding `coding`, ignoring
 * case; "x-gzip" is gzip (RFC 9110 sec. 8.4.1.3). */
static TK_GZIP_TYPE_UNUSED int tk_coding_is(const char *name, size_t n,
                                            const char *coding)
{
    size_t cl = strlen(coding);
    if (n == cl && strncasecmp(name, coding, n) == 0) return 1;
    return strcasecmp(coding, "gzip") == 0 &&
           n == 6 && strncasecmp(name, "x-gzip", 6) == 0;
}

/* 1 when an Accept-Encoding field value admits a response in the content
 * coding `coding` -- "gzip", "br", "zstd", ... (RFC 9110 sec. 12.5.3).
 * Each element is a coding with an optional ";q=" weight, names compared
 * without case.  The coding is acceptable when it is listed with a weight
 * above zero; when it is not listed, when "*" is, with a weight above
 * zero; otherwise not.  So "gzip;q=0" and "*;q=0" refuse gzip, and
 * "gzip;q=0, *" too -- the coding named outright wins over the wildcard.
 * A coding listed twice counts once, at its first weight.  No header (ae
 * NULL) or an empty one admits nothing: the server sends identity.
 * (121.94 wrote this for gzip, whose two writers asked strstr(ae, "gzip"),
 * so "gzip;q=0" -- the client's explicit refusal -- got gzip; 121.96
 * opened it to any coding for content.c's precompressed .br/.gz/.zst
 * lookup, which made the same strstr mistake for all three.) */
static TK_GZIP_TYPE_UNUSED int tk_coding_accepted(const char *ae,
                                                  const char *coding)
{
    if (!ae || !coding) return 0;
    int named = -1, star = -1;   /* -1 unlisted, else 1 / 0 */
    const char *p = ae;
    while (*p) {
        while (*p == ',' || tk_gzip_ows(*p)) p++;
        if (!*p) break;
        const char *name = p;
        while (*p && *p != ',' && *p != ';' && !tk_gzip_ows(*p)) p++;
        size_t nl = (size_t)(p - name);
        int ok = 1;            /* no weight: q=1 */
        while (*p && *p != ',') {
            while (tk_gzip_ows(*p)) p++;
            if (!*p || *p == ',') break;
            if (*p != ';') {   /* junk after the coding: the element is unreadable */
                ok = 0;
                while (*p && *p != ',') p++;
                break;
            }
            p++;
            while (tk_gzip_ows(*p)) p++;
            const char *pn = p;
            while (*p && *p != '=' && *p != ',' && *p != ';' && !tk_gzip_ows(*p)) p++;
            size_t pnl = (size_t)(p - pn);
            while (tk_gzip_ows(*p)) p++;
            const char *pv = p;
            size_t pvl = 0;
            if (*p == '=') {
                p++;
                while (tk_gzip_ows(*p)) p++;
                pv = p;
                while (*p && *p != ',' && *p != ';' && !tk_gzip_ows(*p)) p++;
                pvl = (size_t)(p - pv);
            }
            if (pnl == 1 && (pn[0] == 'q' || pn[0] == 'Q'))
                ok = tk_gzip_qvalue_positive(pv, pvl);
        }
        if (tk_coding_is(name, nl, coding)) {
            if (named < 0) named = ok;
        } else if (nl == 1 && name[0] == '*') {
            if (star < 0) star = ok;
        }
    }
    if (named >= 0) return named;
    return star > 0;
}

/* 1 when an Accept-Encoding field value admits a gzip response
 * (tk_coding_accepted for "gzip"; http.c's and router.c's gzip writers). */
static TK_GZIP_TYPE_UNUSED int tk_gzip_accepted(const char *ae)
{
    return tk_coding_accepted(ae, "gzip");
}

#endif /* TK_GZIP_TYPE_H */
