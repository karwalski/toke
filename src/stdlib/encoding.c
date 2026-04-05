/*
 * encoding.c — Implementation of the std.encoding standard library module.
 *
 * Provides base64 (standard and URL-safe), hex, and URL percent-encoding.
 * All implementations are self-contained; no external dependencies beyond libc.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers.
 *
 * Story: 14.1.1
 */

#include "encoding.h"
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* -----------------------------------------------------------------------
 * UTF-8 validation and rune counting
 * ----------------------------------------------------------------------- */

int encoding_utf8_validate(ByteArray data)
{
    const uint8_t *p   = data.data;
    const uint8_t *end = p + data.len;

    while (p < end) {
        uint8_t b = *p++;
        uint32_t cp;
        int extra;

        if (b < 0x80) {
            /* ASCII fast path */
            continue;
        } else if ((b & 0xe0) == 0xc0) {
            /* 2-byte sequence: 110xxxxx */
            cp    = (uint32_t)(b & 0x1f);
            extra = 1;
        } else if ((b & 0xf0) == 0xe0) {
            /* 3-byte sequence: 1110xxxx */
            cp    = (uint32_t)(b & 0x0f);
            extra = 2;
        } else if ((b & 0xf8) == 0xf0) {
            /* 4-byte sequence: 11110xxx */
            cp    = (uint32_t)(b & 0x07);
            extra = 3;
        } else {
            /* 10xxxxxx continuation without lead, or 0xf8-0xff — invalid */
            return 0;
        }

        /* Consume and validate continuation bytes */
        int i;
        for (i = 0; i < extra; i++) {
            if (p >= end) return 0;           /* truncated */
            uint8_t c = *p++;
            if ((c & 0xc0) != 0x80) return 0; /* not a continuation byte */
            cp = (cp << 6) | (uint32_t)(c & 0x3f);
        }

        /* Reject overlong encodings */
        if (extra == 1 && cp < 0x80)   return 0;
        if (extra == 2 && cp < 0x800)  return 0;
        if (extra == 3 && cp < 0x10000) return 0;

        /* Reject surrogates (U+D800..U+DFFF) */
        if (cp >= 0xd800 && cp <= 0xdfff) return 0;

        /* Reject code points above U+10FFFF */
        if (cp > 0x10ffff) return 0;
    }

    return 1;
}

uint64_t encoding_utf8_rune_count(const char *s)
{
    if (!s) return 0;
    uint64_t count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        /* Count every byte that is NOT a UTF-8 continuation byte (10xxxxxx) */
        if ((*p & 0xc0) != 0x80) count++;
        p++;
    }
    return count;
}

/* -----------------------------------------------------------------------
 * Base32 (RFC 4648)
 * ----------------------------------------------------------------------- */

static const char B32_ALPHA[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

const char *encoding_base32_encode(ByteArray data)
{
    /* Output length: ceil(len/5)*8 chars + NUL */
    uint64_t in_len  = data.len;
    uint64_t out_len = ((in_len + 4) / 5) * 8 + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;

    if (in_len == 0) {
        out[0] = '\0';
        return out;
    }

    const uint8_t *in = data.data;
    uint64_t i = 0, j = 0;

    while (i < in_len) {
        /* Load up to 5 bytes into a 64-bit accumulator */
        uint64_t acc = 0;
        int pad = 0;
        int k;
        for (k = 0; k < 5; k++) {
            acc <<= 8;
            if (i < in_len) {
                acc |= in[i++];
            } else {
                pad++;
            }
        }

        /* Extract 8 groups of 5 bits (most-significant first) */
        out[j+0] = B32_ALPHA[(acc >> 35) & 0x1f];
        out[j+1] = B32_ALPHA[(acc >> 30) & 0x1f];
        out[j+2] = B32_ALPHA[(acc >> 25) & 0x1f];
        out[j+3] = B32_ALPHA[(acc >> 20) & 0x1f];
        out[j+4] = B32_ALPHA[(acc >> 15) & 0x1f];
        out[j+5] = B32_ALPHA[(acc >> 10) & 0x1f];
        out[j+6] = B32_ALPHA[(acc >>  5) & 0x1f];
        out[j+7] = B32_ALPHA[(acc >>  0) & 0x1f];

        /* Overwrite trailing output chars with '=' according to padding needed.
         * RFC 4648: 1 input byte  → 2 b32 chars + 6 '='  (pad=4 → 6)
         *           2 input bytes → 4 b32 chars + 4 '='  (pad=3 → 4)
         *           3 input bytes → 5 b32 chars + 3 '='  (pad=2 → 3)
         *           4 input bytes → 7 b32 chars + 1 '='  (pad=1 → 1)
         *           5 input bytes → 8 b32 chars + 0 '='  (pad=0 → 0) */
        static const int PAD_CHARS[] = {0, 1, 3, 4, 6};
        int pc = PAD_CHARS[pad];
        int n;
        for (n = 0; n < pc; n++) {
            out[j + 7 - n] = '=';
        }

        j += 8;
    }

    out[j] = '\0';
    return out;
}

/* Map a single base32 character (upper or lower) to its 5-bit value, or -1. */
static int b32_char_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a';
    if (c >= '2' && c <= '7') return c - '2' + 26;
    return -1;
}

EncBytesResult encoding_base32_decode(const char *s)
{
    EncBytesResult err;
    err.ok.data  = NULL;
    err.ok.len   = 0;
    err.is_err   = 1;
    err.err_msg  = "invalid base32 input";

    EncBytesResult ok_empty;
    ok_empty.ok.data = NULL;
    ok_empty.ok.len  = 0;
    ok_empty.is_err  = 0;
    ok_empty.err_msg = NULL;

    if (!s) return err;
    if (*s == '\0') return ok_empty;

    uint64_t slen = (uint64_t)strlen(s);

    /* Count non-padding characters and validate */
    uint64_t data_chars = 0;
    uint64_t i;
    for (i = 0; i < slen; i++) {
        if (s[i] == '=') continue; /* skip padding */
        if (b32_char_val(s[i]) == -1) return err;
        data_chars++;
    }

    /* Each 8-char group decodes to 5 bytes; fractional groups: */
    /* data_chars * 5 bits / 8 bits per byte */
    uint64_t out_len = (data_chars * 5) / 8;
    uint8_t *out = (uint8_t *)malloc(out_len + 1);
    if (!out) {
        err.err_msg = "allocation failure";
        return err;
    }

    uint64_t acc_bits = 0;
    uint64_t acc      = 0;
    uint64_t j        = 0;

    for (i = 0; i < slen; i++) {
        if (s[i] == '=') continue;
        int val = b32_char_val(s[i]);
        acc       = (acc << 5) | (uint64_t)val;
        acc_bits += 5;
        if (acc_bits >= 8) {
            acc_bits -= 8;
            out[j++] = (uint8_t)((acc >> acc_bits) & 0xff);
        }
    }

    EncBytesResult result;
    result.ok.data = out;
    result.ok.len  = j;
    result.is_err  = 0;
    result.err_msg = NULL;
    return result;
}

/* -----------------------------------------------------------------------
 * Base64 (RFC 4648)
 * ----------------------------------------------------------------------- */

static const char B64_STD[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static const char B64_URL[]  = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

/* Internal: encode using the given alphabet. pad=1 for standard, pad=0 for URL-safe. */
static const char *b64_encode_impl(ByteArray data, const char *alpha, int pad)
{
    uint64_t in_len  = data.len;
    /* Output length: 4 chars per 3 input bytes, rounded up, plus NUL. */
    uint64_t out_len = ((in_len + 2) / 3) * 4 + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;

    const uint8_t *in = data.data;
    uint64_t i, j = 0;

    for (i = 0; i + 2 < in_len; i += 3) {
        out[j++] = alpha[ (in[i]   >> 2) & 0x3f];
        out[j++] = alpha[((in[i]   << 4) | (in[i+1] >> 4)) & 0x3f];
        out[j++] = alpha[((in[i+1] << 2) | (in[i+2] >> 6)) & 0x3f];
        out[j++] = alpha[  in[i+2]        & 0x3f];
    }

    if (i + 1 == in_len) {
        /* one byte remaining */
        out[j++] = alpha[(in[i] >> 2) & 0x3f];
        out[j++] = alpha[(in[i] << 4) & 0x3f];
        if (pad) { out[j++] = '='; out[j++] = '='; }
    } else if (i + 2 == in_len) {
        /* two bytes remaining */
        out[j++] = alpha[ (in[i]   >> 2) & 0x3f];
        out[j++] = alpha[((in[i]   << 4) | (in[i+1] >> 4)) & 0x3f];
        out[j++] = alpha[ (in[i+1] << 2) & 0x3f];
        if (pad) { out[j++] = '='; }
    }

    out[j] = '\0';
    return out;
}

/* Internal: build a 256-entry decode table for a given alphabet.
 * Returns heap-allocated table; -1 at invalid positions. */
static int *b64_make_table(const char *alpha)
{
    int *table = (int *)malloc(256 * sizeof(int));
    if (!table) return NULL;
    int i;
    for (i = 0; i < 256; i++) table[i] = -1;
    for (i = 0; i < 64; i++) table[(unsigned char)alpha[i]] = i;
    /* Also map padding character to a sentinel (not an error, handled in loop). */
    return table;
}

/* Internal: decode using the given alphabet; accepts optional '=' padding. */
static ByteArray b64_decode_impl(const char *s, const char *alpha)
{
    ByteArray bad = {NULL, 0};
    if (!s) return bad;

    uint64_t slen = (uint64_t)strlen(s);

    int *table = b64_make_table(alpha);
    if (!table) return bad;

    /* Strip trailing '=' for length calculation. */
    uint64_t data_len = slen;
    while (data_len > 0 && s[data_len - 1] == '=') data_len--;

    /* Validate all non-padding characters. */
    uint64_t i;
    for (i = 0; i < data_len; i++) {
        if (table[(unsigned char)s[i]] == -1) {
            free(table);
            return bad;
        }
    }

    /* Calculate output length from data_len (non-padding chars). */
    uint64_t out_len = (data_len * 6) / 8;
    uint8_t *out = (uint8_t *)malloc(out_len + 1);
    if (!out) { free(table); return bad; }

    uint64_t j = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (i = 0; i < data_len; i++) {
        int val = table[(unsigned char)s[i]];
        acc  = (acc << 6) | (uint32_t)val;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[j++] = (uint8_t)((acc >> bits) & 0xff);
        }
    }

    free(table);

    ByteArray result;
    result.data = out;
    result.len  = j;
    return result;
}

const char *encoding_b64encode(ByteArray data)
{
    if (data.len == 0) {
        char *out = (char *)malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    return b64_encode_impl(data, B64_STD, 1);
}

ByteArray encoding_b64decode(const char *s)
{
    return b64_decode_impl(s, B64_STD);
}

const char *encoding_b64urlencode(ByteArray data)
{
    if (data.len == 0) {
        char *out = (char *)malloc(1);
        if (out) out[0] = '\0';
        return out;
    }
    return b64_encode_impl(data, B64_URL, 0);
}

ByteArray encoding_b64urldecode(const char *s)
{
    return b64_decode_impl(s, B64_URL);
}

/* -----------------------------------------------------------------------
 * Hex encoding
 * ----------------------------------------------------------------------- */

static const char HEX_DIGITS[] = "0123456789abcdef";

const char *encoding_hexencode(ByteArray data)
{
    uint64_t out_len = data.len * 2 + 1;
    char *out = (char *)malloc(out_len);
    if (!out) return NULL;

    uint64_t i;
    for (i = 0; i < data.len; i++) {
        out[i * 2]     = HEX_DIGITS[(data.data[i] >> 4) & 0x0f];
        out[i * 2 + 1] = HEX_DIGITS[ data.data[i]       & 0x0f];
    }
    out[data.len * 2] = '\0';
    return out;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

ByteArray encoding_hexdecode(const char *s)
{
    ByteArray bad = {NULL, 0};
    if (!s) return bad;

    uint64_t slen = (uint64_t)strlen(s);
    if (slen % 2 != 0) return bad;  /* odd length is invalid */

    uint64_t out_len = slen / 2;
    uint8_t *out = (uint8_t *)malloc(out_len + 1);
    if (!out) return bad;

    uint64_t i;
    for (i = 0; i < out_len; i++) {
        int hi = hex_val(s[i * 2]);
        int lo = hex_val(s[i * 2 + 1]);
        if (hi == -1 || lo == -1) {
            free(out);
            return bad;
        }
        out[i] = (uint8_t)((hi << 4) | lo);
    }

    ByteArray result;
    result.data = out;
    result.len  = out_len;
    return result;
}

/* -----------------------------------------------------------------------
 * URL percent-encoding (RFC 3986)
 * ----------------------------------------------------------------------- */

/* Unreserved characters per RFC 3986 §2.3: ALPHA / DIGIT / "-" / "." / "_" / "~" */
static int is_unreserved(unsigned char c)
{
    return (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           (c >= '0' && c <= '9') ||
           c == '-' || c == '.' || c == '_' || c == '~';
}

const char *encoding_urlencode(const char *s)
{
    if (!s) return NULL;

    uint64_t slen = (uint64_t)strlen(s);
    /* Worst case: every byte becomes %XX (3 chars). */
    char *out = (char *)malloc(slen * 3 + 1);
    if (!out) return NULL;

    uint64_t i, j = 0;
    for (i = 0; i < slen; i++) {
        unsigned char c = (unsigned char)s[i];
        if (is_unreserved(c)) {
            out[j++] = (char)c;
        } else {
            out[j++] = '%';
            out[j++] = HEX_DIGITS[(c >> 4) & 0x0f];
            out[j++] = HEX_DIGITS[ c        & 0x0f];
        }
    }
    out[j] = '\0';
    return out;
}

const char *encoding_urldecode(const char *s)
{
    if (!s) return NULL;

    uint64_t slen = (uint64_t)strlen(s);
    /* Output is never longer than input. */
    char *out = (char *)malloc(slen + 1);
    if (!out) return NULL;

    uint64_t i = 0, j = 0;
    while (i < slen) {
        if (s[i] == '%' && i + 2 < slen) {
            int hi = hex_val(s[i + 1]);
            int lo = hex_val(s[i + 2]);
            if (hi != -1 && lo != -1) {
                out[j++] = (char)((hi << 4) | lo);
                i += 3;
                continue;
            }
        }
        if (s[i] == '+') {
            out[j++] = ' ';
            i++;
            continue;
        }
        out[j++] = s[i++];
    }
    out[j] = '\0';
    return out;
}
