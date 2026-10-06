/* SPDX-License-Identifier: MIT */
/*
 * encoding_glue.c — i64-ABI wrappers for std.encoding module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.encoding.
 */

#include "encoding.h"
#include "bytes_rt.h"   /* Stage 5: [byte] pack/unpack marshalling */
#include "tk_runtime.h" /* 127.241: tk_current_error + tk_err_box (127.109) */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* 127.241: the error slot (runtime-abi.md §7.2).  The `T!EncodingErr`
 * decoders returned an EMPTY ARRAY for input that is not valid base64 or
 * hex — a real, non-zero handle, so `mt encoding.b64decode("!!")` took the
 * $ok arm with zero bytes and a caller could not tell "decoded to nothing"
 * from "was not base64 at all".  They now store an EncodingErr record {msg}
 * (§7.3) on every failure and clear the slot on every success, so the $err
 * arm is taken and binds `e.msg`.  The core signals invalid input with a NULL
 * data pointer; a valid empty input is a non-NULL zero-length buffer.
 *
 * THE FAILURE VALUE IS UNCHANGED: still an empty array (or "" for
 * urldecode), not 0.  The slot, not the value, is what a `mt` decides on,
 * so the filler is free — and a program that uses a decoder's result bare,
 * without a `mt` (test/standalone does), keeps reading an empty value on bad
 * input rather than a null handle. */
static int64_t enc_raise_v(const char *msg, int64_t filler) {
    int64_t *box = (int64_t *)tk_err_box((int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)msg;
    tk_current_error = (int64_t)(intptr_t)box;
    return filler;
}

static int64_t enc_raise(const char *msg) {
    return enc_raise_v(msg, tk_bytes_pack((const uint8_t *)"", 0));
}

static int64_t enc_raise_str(const char *msg) {
    return enc_raise_v(msg, (int64_t)(intptr_t)"");
}

/* A decoded ByteArray as @(byte), or the failure `why`. */
static int64_t enc_bytes(ByteArray d, const char *why) {
    if (!d.data) return enc_raise(why);
    int64_t out = tk_bytes_pack(d.data, d.len);
    free((void *)d.data);
    if (!out) return enc_raise("encoding: out of memory");
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return out;
}

/* Stage 5 (114.4): real [byte] <-> str/encoding marshalling (was identity
 * stubs). encode takes [byte] and returns a str; decode takes a str and
 * returns [byte]. All binary-safe (0x00 preserved). */
int64_t tk_encoding_tobytes_w(int64_t s) {
    const char *str = s ? (const char *)(intptr_t)s : "";
    return tk_bytes_pack((const uint8_t *)str, (uint64_t)strlen(str));
}
int64_t tk_encoding_bytes_w(int64_t s) { return tk_encoding_tobytes_w(s); }
int64_t tk_encoding_frombytes_w(int64_t b) {
    uint8_t *buf; tk_bytes_unpack(b, &buf);
    return buf ? (int64_t)(intptr_t)buf : (int64_t)(intptr_t)"";
}
int64_t tk_encoding_hexencode_w(int64_t b) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(b, &buf);
    ByteArray ba = { buf, n };
    const char *hex = encoding_hexencode(ba);
    free(buf);
    return hex ? (int64_t)(intptr_t)hex : (int64_t)(intptr_t)"";
}
int64_t tk_encoding_hexdecode_w(int64_t s) {
    if (!s) return enc_raise("encoding.hexdecode: null input");
    return enc_bytes(encoding_hexdecode((const char *)(intptr_t)s),
                     "encoding.hexdecode: not a hex string (odd length, or a non-hex character)");
}
/* 124.2f-a: these two were identity stubs from the 75.12 split and were
 * never wired to encoding.c, so `enc.urlencode("a b")` returned "a b"
 * unchanged. They are str -> str and call the real percent-coders. */
int64_t tk_encoding_urlencode_w(int64_t s) {
    const char *out = encoding_urlencode(s ? (const char *)(intptr_t)s : "");
    return out ? (int64_t)(intptr_t)out : (int64_t)(intptr_t)"";
}
int64_t tk_encoding_urldecode_w(int64_t s) {
    if (!s) return enc_raise_str("encoding.urldecode: null input");
    const char *out = encoding_urldecode((const char *)(intptr_t)s);
    if (!out) return enc_raise_str("encoding.urldecode: out of memory");
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return (int64_t)(intptr_t)out;
}
int64_t tk_encoding_base64encode_w(int64_t b) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(b, &buf);
    ByteArray ba = { buf, n };
    const char *enc = encoding_b64encode(ba);
    free(buf);
    return enc ? (int64_t)(intptr_t)enc : (int64_t)(intptr_t)"";
}
int64_t tk_encoding_base64decode_w(int64_t s) {
    if (!s) return tk_bytes_pack((const uint8_t *)"", 0);
    ByteArray d = encoding_b64decode((const char *)(intptr_t)s);
    int64_t out = tk_bytes_pack(d.data, d.len);
    if (d.data) free((void *)d.data);
    return out;
}
int64_t tk_encoding_toint_w(int64_t s) { return s; }

/* b64encode([byte])->str / b64decode(str)->[byte] — Stage 5 real bytes ABI */
int64_t tk_encoding_b64encode_w(int64_t data) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(data, &buf);
    ByteArray ba = { buf, n };
    const char *enc = encoding_b64encode(ba);
    free(buf);
    return enc ? (int64_t)(intptr_t)enc : (int64_t)(intptr_t)"";
}
int64_t tk_encoding_b64decode_w(int64_t data) {
    if (!data) return enc_raise("encoding.b64decode: null input");
    return enc_bytes(encoding_b64decode((const char *)(intptr_t)data),
                     "encoding.b64decode: not base64 (a character outside the standard alphabet)");
}

/* encoding.b64urldecode(s) -> [byte]!EncodingErr — 127.241.  Declared in
 * stdlib/encoding.tki and implemented in encoding.c, but no wrapper existed,
 * so the export was quarantined (scripts/check_tki_skiplist.txt) and a call
 * failed to link.  URL-safe alphabet, padding optional. */
int64_t tk_encoding_b64urldecode_w(int64_t data) {
    if (!data) return enc_raise("encoding.b64urldecode: null input");
    return enc_bytes(encoding_b64urldecode((const char *)(intptr_t)data),
                     "encoding.b64urldecode: not base64url (a character outside the URL-safe alphabet)");
}

/* base64 (alias spelling) */
int64_t tk_base64_encode_w(int64_t data) { return tk_encoding_b64encode_w(data); }

/* ── Linker-gap additions ───────────────────────────────────────────────── */

/* encoding.base64urlencodenopad([byte]) — URL-safe base64 without padding */
int64_t tk_encoding_base64urlencodenopad_w(int64_t data) {
    uint8_t *buf; uint64_t n = tk_bytes_unpack(data, &buf);
    ByteArray ba = { buf, n };
    const char *enc = encoding_b64urlencode(ba);
    free(buf);
    return enc ? (int64_t)(intptr_t)enc : (int64_t)(intptr_t)"";
}

/* encoding.jsonfield(obj, key) — extract a JSON string field value.
 * Simple implementation: find "key":"value" and return value. */
int64_t tk_encoding_jsonfield_w(int64_t json_str, int64_t key) {
    if (!json_str || !key) return 0;
    const char *j = (const char *)(intptr_t)json_str;
    const char *k = (const char *)(intptr_t)key;
    size_t klen = strlen(k);
    /* Search for "key":" pattern */
    const char *p = j;
    while ((p = strstr(p, k)) != NULL) {
        /* Verify it's a proper key: preceded by " and followed by ": */
        if (p > j && *(p - 1) == '"') {
            const char *after = p + klen;
            if (*after == '"' && *(after + 1) == ':') {
                const char *vstart = after + 2;
                while (*vstart == ' ') vstart++;
                if (*vstart == '"') {
                    vstart++;
                    const char *vend = vstart;
                    while (*vend && *vend != '"') {
                        if (*vend == '\\') vend++; /* skip escaped char */
                        vend++;
                    }
                    size_t vlen = (size_t)(vend - vstart);
                    char *result = (char *)malloc(vlen + 1);
                    if (!result) return 0;
                    memcpy(result, vstart, vlen);
                    result[vlen] = '\0';
                    return (int64_t)(intptr_t)result;
                }
            }
        }
        p++;
    }
    return 0;
}

/* encoding.jsonfieldint(obj, key) — extract a JSON integer field value */
int64_t tk_encoding_jsonfieldint_w(int64_t json_str, int64_t key) {
    if (!json_str || !key) return 0;
    const char *j = (const char *)(intptr_t)json_str;
    const char *k = (const char *)(intptr_t)key;
    size_t klen = strlen(k);
    const char *p = j;
    while ((p = strstr(p, k)) != NULL) {
        if (p > j && *(p - 1) == '"') {
            const char *after = p + klen;
            if (*after == '"' && *(after + 1) == ':') {
                const char *vstart = after + 2;
                while (*vstart == ' ') vstart++;
                /* Parse integer directly */
                char *end;
                long long val = strtoll(vstart, &end, 10);
                if (end != vstart) return (int64_t)val;
            }
        }
        p++;
    }
    return 0;
}

/*
 * Story 136.32 — encoding.b64urlencode([byte]) : $str.
 *
 * encoding_b64urlencode() has always been in encoding.c, and the wrapper
 * above (tk_encoding_base64urlencodenopad_w) already calls it — under a name
 * stdlib/encoding.tki does not export. The DOCUMENTED spelling,
 * `enc.b64urlencode`, had no symbol, so the example on
 * docs/stdlib/encoding.md failed at link one line after a b64encode that
 * worked.
 */
int64_t tk_encoding_b64urlencode_w(int64_t data) {
    return tk_encoding_base64urlencodenopad_w(data);
}
