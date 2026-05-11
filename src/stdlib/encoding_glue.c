/*
 * encoding_glue.c — i64-ABI wrappers for std.encoding module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.encoding.
 */

#include <stdint.h>

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
int64_t tk_base64_encode_w(int64_t data) { (void)data; return 0; }
int64_t tk_base64_decode_w(int64_t data) { (void)data; return 0; }
