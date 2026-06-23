/*
 * encoding_glue.c — i64-ABI wrappers for std.encoding module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.encoding.
 */

#include "encoding.h"
#include "bytes_rt.h"   /* Stage 5: [byte] pack/unpack marshalling */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

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
    if (!s) return tk_bytes_pack((const uint8_t *)"", 0);
    ByteArray d = encoding_hexdecode((const char *)(intptr_t)s);
    int64_t out = tk_bytes_pack(d.data, d.len);
    if (d.data) free((void *)d.data);
    return out;
}
int64_t tk_encoding_urlencode_w(int64_t s) { return s; }   /* str-domain; unchanged */
int64_t tk_encoding_urldecode_w(int64_t s) { return s; }
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
    if (!data) return tk_bytes_pack((const uint8_t *)"", 0);
    ByteArray d = encoding_b64decode((const char *)(intptr_t)data);
    int64_t out = tk_bytes_pack(d.data, d.len);
    if (d.data) free((void *)d.data);
    return out;
}

/* base64 (alias spelling) */
int64_t tk_base64_encode_w(int64_t data) { return tk_encoding_b64encode_w(data); }
int64_t tk_base64_decode_w(int64_t data) { return tk_encoding_b64decode_w(data); }

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
