/*
 * test_encoding.c — Unit tests for the std.encoding C library (Story 14.1.1).
 *
 * Build and run: make test-stdlib-encoding
 *
 * Known-vector sources (verified with Python3):
 *   base64.b64encode(b"")    = b""
 *   base64.b64encode(b"Man") = b"TWFu"
 *   base64.b64encode(b"Ma")  = b"TWE="
 *   base64.b64encode(b"M")   = b"TQ=="
 *   bytes.hex(b"\x00\xff\xab") = "00ffab"
 *   urllib.parse.quote("hello world") = "hello%20world"
 */

#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/encoding.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

/* Build a ByteArray from a string literal (no NUL included). */
static ByteArray ba_from_str(const char *s)
{
    ByteArray b;
    b.data = (const uint8_t *)s;
    b.len  = (uint64_t)strlen(s);
    return b;
}

/* Build a ByteArray from raw bytes. */
static ByteArray ba_from_bytes(const uint8_t *data, uint64_t len)
{
    ByteArray b;
    b.data = data;
    b.len  = len;
    return b;
}

int main(void)
{
    /* ------------------------------------------------------------------ */
    /* Base64 encode — known vectors                                        */
    /* ------------------------------------------------------------------ */

    ByteArray empty = {NULL, 0};
    const char *b64_empty = encoding_b64encode(empty);
    ASSERT(b64_empty && strlen(b64_empty) == 0, "b64encode(empty) == \"\"");

    ByteArray man = ba_from_str("Man");
    const char *b64_man = encoding_b64encode(man);
    ASSERT_STREQ(b64_man, "TWFu", "b64encode(\"Man\") == \"TWFu\"");

    ByteArray ma = ba_from_str("Ma");
    const char *b64_ma = encoding_b64encode(ma);
    ASSERT_STREQ(b64_ma, "TWE=", "b64encode(\"Ma\") == \"TWE=\"");

    ByteArray m = ba_from_str("M");
    const char *b64_m = encoding_b64encode(m);
    ASSERT_STREQ(b64_m, "TQ==", "b64encode(\"M\") == \"TQ==\"");

    /* ------------------------------------------------------------------ */
    /* Base64 decode — round-trip                                           */
    /* ------------------------------------------------------------------ */

    ByteArray dec_man = encoding_b64decode("TWFu");
    ASSERT(dec_man.len == 3 &&
           dec_man.data[0] == 'M' &&
           dec_man.data[1] == 'a' &&
           dec_man.data[2] == 'n',
           "b64decode(\"TWFu\") == \"Man\"");

    ByteArray dec_ma = encoding_b64decode("TWE=");
    ASSERT(dec_ma.len == 2 &&
           dec_ma.data[0] == 'M' &&
           dec_ma.data[1] == 'a',
           "b64decode(\"TWE=\") == \"Ma\"");

    ByteArray dec_m = encoding_b64decode("TQ==");
    ASSERT(dec_m.len == 1 && dec_m.data[0] == 'M',
           "b64decode(\"TQ==\") == \"M\"");

    /* Round-trip a longer string. */
    ByteArray hello = ba_from_str("Hello, World!");
    const char *b64_hello = encoding_b64encode(hello);
    ByteArray dec_hello = encoding_b64decode(b64_hello);
    ASSERT(dec_hello.len == hello.len &&
           memcmp(dec_hello.data, hello.data, hello.len) == 0,
           "b64 round-trip \"Hello, World!\"");

    /* Invalid input returns len==0. */
    ByteArray bad_b64 = encoding_b64decode("not!valid@base64#");
    ASSERT(bad_b64.len == 0, "b64decode(invalid) len==0");

    /* ------------------------------------------------------------------ */
    /* URL-safe base64                                                      */
    /* ------------------------------------------------------------------ */

    /* Encode bytes that produce '+' and '/' in standard base64.
     * 0xfb = 11111011 -> last 6 bits cross boundary → produces '+' in std.
     * Using \xfb\xff to force characters from the high part of the alphabet. */
    const uint8_t url_bytes[] = {0xfb, 0xff, 0xfe};
    ByteArray url_ba = ba_from_bytes(url_bytes, 3);

    const char *b64std_out  = encoding_b64encode(url_ba);
    const char *b64url_out  = encoding_b64urlencode(url_ba);

    /* Standard must contain '+' or '/' somewhere; URL-safe must not. */
    ASSERT(b64std_out && (strchr(b64std_out, '+') || strchr(b64std_out, '/')),
           "b64encode of 0xfb/0xff/0xfe contains '+' or '/'");
    ASSERT(b64url_out && !strchr(b64url_out, '+') && !strchr(b64url_out, '/'),
           "b64urlencode of 0xfb/0xff/0xfe has no '+' or '/'");
    ASSERT(b64url_out && !strchr(b64url_out, '='),
           "b64urlencode has no padding '='");

    /* URL-safe round-trip. */
    ByteArray dec_url = encoding_b64urldecode(b64url_out);
    ASSERT(dec_url.len == 3 && memcmp(dec_url.data, url_bytes, 3) == 0,
           "b64url round-trip 0xfb/0xff/0xfe");

    /* ------------------------------------------------------------------ */
    /* Hex encode / decode                                                  */
    /* ------------------------------------------------------------------ */

    const uint8_t hex_bytes[] = {0x00, 0xff, 0xab};
    ByteArray hex_ba = ba_from_bytes(hex_bytes, 3);
    const char *hexstr = encoding_hexencode(hex_ba);
    ASSERT_STREQ(hexstr, "00ffab", "hexencode({0x00,0xff,0xab}) == \"00ffab\"");

    /* Round-trip deadbeef. */
    ByteArray dead = encoding_hexdecode("deadbeef");
    ASSERT(dead.len == 4, "hexdecode(\"deadbeef\") len==4");
    const char *dead_hex = encoding_hexencode(dead);
    ASSERT_STREQ(dead_hex, "deadbeef", "hex round-trip \"deadbeef\"");

    /* Uppercase input accepted. */
    ByteArray upper = encoding_hexdecode("DEADBEEF");
    ASSERT(upper.len == 4 && memcmp(upper.data, dead.data, 4) == 0,
           "hexdecode accepts uppercase");

    /* Odd-length returns len==0. */
    ByteArray bad_hex = encoding_hexdecode("abc");
    ASSERT(bad_hex.len == 0, "hexdecode(odd length) len==0");

    /* Non-hex characters return len==0. */
    ByteArray bad_hex2 = encoding_hexdecode("zz");
    ASSERT(bad_hex2.len == 0, "hexdecode(non-hex chars) len==0");

    /* ------------------------------------------------------------------ */
    /* URL percent-encoding                                                 */
    /* ------------------------------------------------------------------ */

    const char *enc_space = encoding_urlencode("hello world");
    ASSERT_STREQ(enc_space, "hello%20world", "urlencode(\"hello world\")");

    const char *enc_eq_amp = encoding_urlencode("a=b&c=d");
    ASSERT(enc_eq_amp &&
           strstr(enc_eq_amp, "%3D") && strstr(enc_eq_amp, "%26"),
           "urlencode encodes '=' and '&'");

    /* Unreserved characters must pass through unchanged. */
    const char *enc_unreserved = encoding_urlencode("abc-._~");
    ASSERT_STREQ(enc_unreserved, "abc-._~", "urlencode leaves unreserved chars alone");

    /* ------------------------------------------------------------------ */
    /* URL percent-decoding                                                 */
    /* ------------------------------------------------------------------ */

    const char *dec_hello_pct = encoding_urldecode("%48%65%6C%6C%6F");
    ASSERT_STREQ(dec_hello_pct, "Hello", "urldecode(\"%48%65%6C%6C%6F\") == \"Hello\"");

    const char *dec_plus = encoding_urldecode("hello+world");
    ASSERT_STREQ(dec_plus, "hello world", "urldecode(\"hello+world\") == \"hello world\"");

    /* Round-trip urlencode → urldecode. */
    const char *original    = "foo bar/baz?key=value&other=1";
    const char *encoded_rt  = encoding_urlencode(original);
    const char *decoded_rt  = encoding_urldecode(encoded_rt);
    ASSERT_STREQ(decoded_rt, original, "url round-trip");

    /* ------------------------------------------------------------------ */
    /* Summary                                                              */
    /* ------------------------------------------------------------------ */

    if (failures == 0) { printf("All encoding tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
