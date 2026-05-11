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
