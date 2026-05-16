/*
 * encoding_glue.c — i64-ABI wrappers for std.encoding module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.encoding.
 */

#include "encoding.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int64_t tk_encoding_tobytes_w(int64_t s) { return s; }
int64_t tk_encoding_frombytes_w(int64_t b) { return b; }
int64_t tk_encoding_hexencode_w(int64_t s) { return s; }
int64_t tk_encoding_hexdecode_w(int64_t s) { return s; }
int64_t tk_encoding_urlencode_w(int64_t s) { return s; }
int64_t tk_encoding_urldecode_w(int64_t s) { return s; }
int64_t tk_encoding_base64encode_w(int64_t s) { return s; }
int64_t tk_encoding_base64decode_w(int64_t s) { return s; }
int64_t tk_encoding_bytes_w(int64_t s) { return s; }
int64_t tk_encoding_toint_w(int64_t s) { return s; }

/* b64encode/b64decode — string-oriented wrappers matching toke enc.b64encode() */
int64_t tk_encoding_b64encode_w(int64_t data) {
    if (!data) return 0;
    const char *s = (const char *)(intptr_t)data;
    ByteArray ba = { (const uint8_t *)s, (uint64_t)strlen(s) };
    const char *encoded = encoding_b64encode(ba);
    return encoded ? (int64_t)(intptr_t)encoded : 0;
}
int64_t tk_encoding_b64decode_w(int64_t data) {
    if (!data) return 0;
    const char *s = (const char *)(intptr_t)data;
    ByteArray decoded = encoding_b64decode(s);
    if (!decoded.data || decoded.len == 0) return 0;
    char *str = (char *)malloc(decoded.len + 1);
    if (!str) return 0;
    memcpy(str, decoded.data, decoded.len);
    str[decoded.len] = '\0';
    free((void *)decoded.data);
    return (int64_t)(intptr_t)str;
}

/* base64 */
int64_t tk_base64_encode_w(int64_t data) {
    if (!data) return 0;
    const char *s = (const char *)(intptr_t)data;
    ByteArray ba = { (const uint8_t *)s, (uint64_t)strlen(s) };
    const char *encoded = encoding_b64encode(ba);
    return encoded ? (int64_t)(intptr_t)encoded : 0;
}
int64_t tk_base64_decode_w(int64_t data) {
    if (!data) return 0;
    const char *s = (const char *)(intptr_t)data;
    ByteArray decoded = encoding_b64decode(s);
    if (!decoded.data || decoded.len == 0) return 0;
    /* Return as NUL-terminated string */
    char *str = (char *)malloc(decoded.len + 1);
    if (!str) return 0;
    memcpy(str, decoded.data, decoded.len);
    str[decoded.len] = '\0';
    free((void *)decoded.data);
    return (int64_t)(intptr_t)str;
}

/* ── Linker-gap additions ───────────────────────────────────────────────── */

/* encoding.base64urlencodenopad(data) — URL-safe base64 without padding */
int64_t tk_encoding_base64urlencodenopad_w(int64_t data) {
    if (!data) return 0;
    const char *s = (const char *)(intptr_t)data;
    ByteArray ba = { (const uint8_t *)s, (uint64_t)strlen(s) };
    const char *encoded = encoding_b64urlencode(ba);
    return encoded ? (int64_t)(intptr_t)encoded : 0;
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
