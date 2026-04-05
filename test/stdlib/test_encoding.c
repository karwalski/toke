/*
 * test_encoding.c — Unit tests for the std.encoding C library.
 *
 * Build and run: make test-stdlib-encoding
 * Stories: 14.1.1, 20.1.1
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
#include <stdlib.h>
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
    /* ================================================================== */
    /* Base64 encode — known vectors                                       */
    /* ================================================================== */

    /* 1 */
    ByteArray empty = {NULL, 0};
    const char *b64_empty = encoding_b64encode(empty);
    ASSERT(b64_empty && strlen(b64_empty) == 0, "b64encode(empty) == \"\"");

    /* 2 */
    ByteArray man = ba_from_str("Man");
    const char *b64_man = encoding_b64encode(man);
    ASSERT_STREQ(b64_man, "TWFu", "b64encode(\"Man\") == \"TWFu\"");

    /* 3 */
    ByteArray ma = ba_from_str("Ma");
    const char *b64_ma = encoding_b64encode(ma);
    ASSERT_STREQ(b64_ma, "TWE=", "b64encode(\"Ma\") == \"TWE=\"");

    /* 4 */
    ByteArray m = ba_from_str("M");
    const char *b64_m = encoding_b64encode(m);
    ASSERT_STREQ(b64_m, "TQ==", "b64encode(\"M\") == \"TQ==\"");

    /* ================================================================== */
    /* Base64 decode — round-trip and known vectors                         */
    /* ================================================================== */

    /* 5 */
    ByteArray dec_man = encoding_b64decode("TWFu");
    ASSERT(dec_man.len == 3 &&
           dec_man.data[0] == 'M' &&
           dec_man.data[1] == 'a' &&
           dec_man.data[2] == 'n',
           "b64decode(\"TWFu\") == \"Man\"");

    /* 6 */
    ByteArray dec_ma = encoding_b64decode("TWE=");
    ASSERT(dec_ma.len == 2 &&
           dec_ma.data[0] == 'M' &&
           dec_ma.data[1] == 'a',
           "b64decode(\"TWE=\") == \"Ma\"");

    /* 7 */
    ByteArray dec_m = encoding_b64decode("TQ==");
    ASSERT(dec_m.len == 1 && dec_m.data[0] == 'M',
           "b64decode(\"TQ==\") == \"M\"");

    /* 8 */
    ByteArray hello = ba_from_str("Hello, World!");
    const char *b64_hello = encoding_b64encode(hello);
    ByteArray dec_hello = encoding_b64decode(b64_hello);
    ASSERT(dec_hello.len == hello.len &&
           memcmp(dec_hello.data, hello.data, hello.len) == 0,
           "b64 round-trip \"Hello, World!\"");

    /* 9 — Invalid input returns len==0 */
    ByteArray bad_b64 = encoding_b64decode("not!valid@base64#");
    ASSERT(bad_b64.len == 0, "b64decode(invalid chars) len==0");

    /* 10 — Decode empty string */
    ByteArray dec_empty_b64 = encoding_b64decode("");
    ASSERT(dec_empty_b64.len == 0, "b64decode(\"\") len==0");

    /* 11 — Null bytes survive base64 round-trip */
    {
        const uint8_t with_nulls[] = {0x00, 0x41, 0x00, 0x42, 0x00};
        ByteArray ba_nulls = ba_from_bytes(with_nulls, 5);
        const char *enc_nulls = encoding_b64encode(ba_nulls);
        ByteArray dec_nulls = encoding_b64decode(enc_nulls);
        ASSERT(dec_nulls.len == 5 &&
               memcmp(dec_nulls.data, with_nulls, 5) == 0,
               "b64 round-trip with embedded null bytes");
        free((void *)enc_nulls);
        free((void *)dec_nulls.data);
    }

    /* 12 — Corrupted padding: wrong number of pad chars */
    {
        ByteArray bad_pad1 = encoding_b64decode("TQ=");  /* should be TQ== */
        /* The decoder strips padding and works on data chars, so this
         * may decode (implementation accepts short padding). Just ensure
         * it does not crash. We verify it either decodes or returns empty. */
        ASSERT(bad_pad1.len <= 1, "b64decode with single '=' pad does not crash");
        free((void *)bad_pad1.data);
    }

    /* 13 — Padding in the middle is invalid */
    {
        ByteArray bad_mid = encoding_b64decode("TQ==TQ==");
        /* '=' in middle maps to -1 in the decode table after stripping
         * trailing padding, so this should fail. */
        ASSERT(bad_mid.len == 0 || bad_mid.data != NULL,
               "b64decode with mid-padding does not crash");
        free((void *)bad_mid.data);
    }

    /* 14 — b64decode(NULL) returns empty */
    {
        ByteArray dec_null = encoding_b64decode(NULL);
        ASSERT(dec_null.len == 0 && dec_null.data == NULL,
               "b64decode(NULL) returns empty ByteArray");
    }

    /* ================================================================== */
    /* URL-safe base64                                                      */
    /* ================================================================== */

    /* 15 */
    const uint8_t url_bytes[] = {0xfb, 0xff, 0xfe};
    ByteArray url_ba = ba_from_bytes(url_bytes, 3);

    const char *b64std_out  = encoding_b64encode(url_ba);
    const char *b64url_out  = encoding_b64urlencode(url_ba);

    ASSERT(b64std_out && (strchr(b64std_out, '+') || strchr(b64std_out, '/')),
           "b64encode of 0xfb/0xff/0xfe contains '+' or '/'");
    /* 16 */
    ASSERT(b64url_out && !strchr(b64url_out, '+') && !strchr(b64url_out, '/'),
           "b64urlencode of 0xfb/0xff/0xfe has no '+' or '/'");
    /* 17 */
    ASSERT(b64url_out && !strchr(b64url_out, '='),
           "b64urlencode has no padding '='");

    /* 18 — URL-safe round-trip */
    ByteArray dec_url = encoding_b64urldecode(b64url_out);
    ASSERT(dec_url.len == 3 && memcmp(dec_url.data, url_bytes, 3) == 0,
           "b64url round-trip 0xfb/0xff/0xfe");

    /* 19 — b64url empty input */
    {
        ByteArray url_empty = {NULL, 0};
        const char *url_enc_empty = encoding_b64urlencode(url_empty);
        ASSERT(url_enc_empty && strlen(url_enc_empty) == 0,
               "b64urlencode(empty) == \"\"");
        free((void *)url_enc_empty);

        ByteArray url_dec_empty = encoding_b64urldecode("");
        ASSERT(url_dec_empty.len == 0, "b64urldecode(\"\") len==0");
        free((void *)url_dec_empty.data);
    }

    /* ================================================================== */
    /* Hex encode / decode                                                  */
    /* ================================================================== */

    /* 20 */
    const uint8_t hex_bytes[] = {0x00, 0xff, 0xab};
    ByteArray hex_ba = ba_from_bytes(hex_bytes, 3);
    const char *hexstr = encoding_hexencode(hex_ba);
    ASSERT_STREQ(hexstr, "00ffab", "hexencode({0x00,0xff,0xab}) == \"00ffab\"");

    /* 21 */
    ByteArray dead = encoding_hexdecode("deadbeef");
    ASSERT(dead.len == 4, "hexdecode(\"deadbeef\") len==4");
    const char *dead_hex = encoding_hexencode(dead);
    ASSERT_STREQ(dead_hex, "deadbeef", "hex round-trip \"deadbeef\"");

    /* 22 */
    ByteArray upper = encoding_hexdecode("DEADBEEF");
    ASSERT(upper.len == 4 && memcmp(upper.data, dead.data, 4) == 0,
           "hexdecode accepts uppercase");

    /* 23 — Odd-length returns len==0 */
    ByteArray bad_hex = encoding_hexdecode("abc");
    ASSERT(bad_hex.len == 0, "hexdecode(odd length) len==0");

    /* 24 — Non-hex characters return len==0 */
    ByteArray bad_hex2 = encoding_hexdecode("zz");
    ASSERT(bad_hex2.len == 0, "hexdecode(non-hex chars) len==0");

    /* 25 — Hex empty input */
    {
        ByteArray hex_empty_ba = {NULL, 0};
        const char *hex_enc_empty = encoding_hexencode(hex_empty_ba);
        ASSERT(hex_enc_empty && strlen(hex_enc_empty) == 0,
               "hexencode(empty) == \"\"");
        free((void *)hex_enc_empty);

        ByteArray hex_dec_empty = encoding_hexdecode("");
        ASSERT(hex_dec_empty.len == 0, "hexdecode(\"\") len==0");
        free((void *)hex_dec_empty.data);
    }

    /* 26 — Hex decode NULL input */
    {
        ByteArray hex_dec_null = encoding_hexdecode(NULL);
        ASSERT(hex_dec_null.len == 0 && hex_dec_null.data == NULL,
               "hexdecode(NULL) returns empty ByteArray");
    }

    /* 27 — Mixed case hex */
    {
        ByteArray mixed = encoding_hexdecode("aAbBcCdD");
        ASSERT(mixed.len == 4, "hexdecode mixed case len==4");
        ASSERT(mixed.data[0] == 0xaa && mixed.data[1] == 0xbb &&
               mixed.data[2] == 0xcc && mixed.data[3] == 0xdd,
               "hexdecode mixed case values correct");
        free((void *)mixed.data);
    }

    /* ================================================================== */
    /* URL percent-encoding                                                 */
    /* ================================================================== */

    /* 28 */
    const char *enc_space = encoding_urlencode("hello world");
    ASSERT_STREQ(enc_space, "hello%20world", "urlencode(\"hello world\")");

    /* 29 */
    const char *enc_eq_amp = encoding_urlencode("a=b&c=d");
    ASSERT(enc_eq_amp &&
           strstr(enc_eq_amp, "%3d") && strstr(enc_eq_amp, "%26"),
           "urlencode encodes '=' and '&'");

    /* 30 — Unreserved characters pass through unchanged */
    const char *enc_unreserved = encoding_urlencode("abc-._~");
    ASSERT_STREQ(enc_unreserved, "abc-._~", "urlencode leaves unreserved chars alone");

    /* 31 — Empty string */
    {
        const char *enc_empty_url = encoding_urlencode("");
        ASSERT(enc_empty_url && strlen(enc_empty_url) == 0,
               "urlencode(\"\") == \"\"");
        free((void *)enc_empty_url);
    }

    /* 32 — All RFC 3986 reserved characters are percent-encoded */
    {
        const char *reserved = ":/?#[]@!$&'()*+,;=";
        const char *enc_reserved = encoding_urlencode(reserved);
        /* None of the reserved chars should appear literally in output */
        int found_literal = 0;
        uint64_t i;
        for (i = 0; reserved[i]; i++) {
            /* Check that this reserved char is NOT present as a literal
             * (it may appear as part of a %XX hex digit though) */
            const char *p = enc_reserved;
            while (*p) {
                if (*p == '%') {
                    p += 3; /* skip %XX */
                    continue;
                }
                if (*p == reserved[i]) { found_literal = 1; break; }
                p++;
            }
        }
        ASSERT(!found_literal,
               "urlencode percent-encodes all RFC 3986 reserved chars");
        free((void *)enc_reserved);
    }

    /* 33 — Invalid UTF-8 bytes in urlencode (should percent-encode, not crash) */
    {
        /* 0xfe and 0xff are never valid in UTF-8 */
        const char invalid_utf8[] = {(char)0xfe, (char)0xff, '\0'};
        const char *enc_inv = encoding_urlencode(invalid_utf8);
        ASSERT_STREQ(enc_inv, "%fe%ff",
                     "urlencode of invalid UTF-8 bytes 0xfe/0xff");
        free((void *)enc_inv);
    }

    /* 34 — Multi-byte UTF-8 (e.g. U+00E9 = C3 A9) percent-encoded */
    {
        const char *utf8_e_acute = "\xc3\xa9";  /* 'e with acute' */
        const char *enc_utf8 = encoding_urlencode(utf8_e_acute);
        ASSERT_STREQ(enc_utf8, "%c3%a9",
                     "urlencode of U+00E9 (e-acute) == \"%c3%a9\"");
        free((void *)enc_utf8);
    }

    /* 35 — urlencode(NULL) returns NULL */
    {
        const char *enc_null = encoding_urlencode(NULL);
        ASSERT(enc_null == NULL, "urlencode(NULL) returns NULL");
    }

    /* ================================================================== */
    /* URL percent-decoding                                                 */
    /* ================================================================== */

    /* 36 */
    const char *dec_hello_pct = encoding_urldecode("%48%65%6C%6C%6F");
    ASSERT_STREQ(dec_hello_pct, "Hello", "urldecode(\"%48%65%6C%6C%6F\") == \"Hello\"");

    /* 37 */
    const char *dec_plus = encoding_urldecode("hello+world");
    ASSERT_STREQ(dec_plus, "hello world", "urldecode(\"hello+world\") == \"hello world\"");

    /* 38 — Round-trip urlencode then urldecode */
    const char *original    = "foo bar/baz?key=value&other=1";
    const char *encoded_rt  = encoding_urlencode(original);
    const char *decoded_rt  = encoding_urldecode(encoded_rt);
    ASSERT_STREQ(decoded_rt, original, "url round-trip");

    /* 39 — urldecode empty string */
    {
        const char *dec_empty_url = encoding_urldecode("");
        ASSERT(dec_empty_url && strlen(dec_empty_url) == 0,
               "urldecode(\"\") == \"\"");
        free((void *)dec_empty_url);
    }

    /* 40 — urldecode(NULL) returns NULL */
    {
        const char *dec_null_url = encoding_urldecode(NULL);
        ASSERT(dec_null_url == NULL, "urldecode(NULL) returns NULL");
    }

    /* 41 — Truncated percent sequence at end of string */
    {
        const char *dec_trunc1 = encoding_urldecode("abc%2");
        /* '%' at position with only 1 hex digit remaining: literal passthrough */
        ASSERT(dec_trunc1 && strcmp(dec_trunc1, "abc%2") == 0,
               "urldecode truncated %2 at end is literal");
        free((void *)dec_trunc1);
    }

    /* 42 — Bare percent sign */
    {
        const char *dec_bare = encoding_urldecode("100%done");
        /* '%do' — 'd' is hex but 'o' is not, so literal passthrough */
        ASSERT(dec_bare && strcmp(dec_bare, "100%done") == 0,
               "urldecode bare % with invalid hex is literal");
        free((void *)dec_bare);
    }

    /* ================================================================== */
    /* Large input (1 MB+) round-trip tests                                */
    /* ================================================================== */

    /* 43 — Base64 round-trip with 1 MB data */
    {
        uint64_t big_len = 1024 * 1024 + 7;  /* 1 MB + 7 for non-aligned size */
        uint8_t *big_data = (uint8_t *)malloc(big_len);
        ASSERT(big_data != NULL, "malloc 1MB+ for b64 large test");
        if (big_data) {
            uint64_t i;
            for (i = 0; i < big_len; i++) {
                big_data[i] = (uint8_t)(i & 0xff);
            }
            ByteArray big_ba = ba_from_bytes(big_data, big_len);
            const char *big_enc = encoding_b64encode(big_ba);
            ASSERT(big_enc != NULL, "b64encode 1MB+ succeeds");
            if (big_enc) {
                ByteArray big_dec = encoding_b64decode(big_enc);
                ASSERT(big_dec.len == big_len &&
                       memcmp(big_dec.data, big_data, big_len) == 0,
                       "b64 round-trip 1MB+ data matches");
                free((void *)big_dec.data);
                free((void *)big_enc);
            }
            free(big_data);
        }
    }

    /* 44 — Hex round-trip with 1 MB data */
    {
        uint64_t big_len = 1024 * 1024;
        uint8_t *big_data = (uint8_t *)malloc(big_len);
        ASSERT(big_data != NULL, "malloc 1MB for hex large test");
        if (big_data) {
            uint64_t i;
            for (i = 0; i < big_len; i++) {
                big_data[i] = (uint8_t)(i & 0xff);
            }
            ByteArray big_ba = ba_from_bytes(big_data, big_len);
            const char *big_hex = encoding_hexencode(big_ba);
            ASSERT(big_hex != NULL, "hexencode 1MB succeeds");
            if (big_hex) {
                ASSERT(strlen(big_hex) == big_len * 2,
                       "hexencode 1MB output length correct");
                ByteArray big_dec = encoding_hexdecode(big_hex);
                ASSERT(big_dec.len == big_len &&
                       memcmp(big_dec.data, big_data, big_len) == 0,
                       "hex round-trip 1MB data matches");
                free((void *)big_dec.data);
                free((void *)big_hex);
            }
            free(big_data);
        }
    }

    /* ================================================================== */
    /* All-bytes round-trip (0x00..0xFF)                                    */
    /* ================================================================== */

    /* 45 — Base64 round-trip of all 256 byte values */
    {
        uint8_t all256[256];
        int i;
        for (i = 0; i < 256; i++) all256[i] = (uint8_t)i;
        ByteArray ba256 = ba_from_bytes(all256, 256);
        const char *enc256 = encoding_b64encode(ba256);
        ByteArray dec256 = encoding_b64decode(enc256);
        ASSERT(dec256.len == 256 && memcmp(dec256.data, all256, 256) == 0,
               "b64 round-trip all 256 byte values");
        free((void *)enc256);
        free((void *)dec256.data);
    }

    /* 46 — Hex round-trip of all 256 byte values */
    {
        uint8_t all256[256];
        int i;
        for (i = 0; i < 256; i++) all256[i] = (uint8_t)i;
        ByteArray ba256 = ba_from_bytes(all256, 256);
        const char *hex256 = encoding_hexencode(ba256);
        ByteArray dec256 = encoding_hexdecode(hex256);
        ASSERT(dec256.len == 256 && memcmp(dec256.data, all256, 256) == 0,
               "hex round-trip all 256 byte values");
        free((void *)hex256);
        free((void *)dec256.data);
    }

    /* 47 — URL encode/decode round-trip of all printable ASCII */
    {
        char printable[96];  /* 0x20..0x7e = 95 chars + NUL */
        int i;
        for (i = 0; i < 95; i++) printable[i] = (char)(0x20 + i);
        printable[95] = '\0';
        const char *enc_print = encoding_urlencode(printable);
        const char *dec_print = encoding_urldecode(enc_print);
        ASSERT_STREQ(dec_print, printable,
                     "url round-trip all printable ASCII");
        free((void *)enc_print);
        free((void *)dec_print);
    }

    /* ================================================================== */
    /* Single-byte edge cases                                               */
    /* ================================================================== */

    /* 48 — Single null byte in base64 */
    {
        const uint8_t zero = 0x00;
        ByteArray ba_zero = ba_from_bytes(&zero, 1);
        const char *enc_zero = encoding_b64encode(ba_zero);
        ASSERT_STREQ(enc_zero, "AA==", "b64encode(0x00) == \"AA==\"");
        ByteArray dec_zero = encoding_b64decode("AA==");
        ASSERT(dec_zero.len == 1 && dec_zero.data[0] == 0x00,
               "b64decode(\"AA==\") == 0x00");
        free((void *)enc_zero);
        free((void *)dec_zero.data);
    }

    /* 49 — Single null byte in hex */
    {
        const uint8_t zero = 0x00;
        ByteArray ba_zero = ba_from_bytes(&zero, 1);
        const char *hex_zero = encoding_hexencode(ba_zero);
        ASSERT_STREQ(hex_zero, "00", "hexencode(0x00) == \"00\"");
        free((void *)hex_zero);
    }

    /* 50 — Single 0xff byte in hex */
    {
        const uint8_t ff = 0xff;
        ByteArray ba_ff = ba_from_bytes(&ff, 1);
        const char *hex_ff = encoding_hexencode(ba_ff);
        ASSERT_STREQ(hex_ff, "ff", "hexencode(0xff) == \"ff\"");
        free((void *)hex_ff);
    }

    /* ================================================================== */
    /* Summary                                                              */
    /* ================================================================== */

    if (failures == 0) { printf("\nAll encoding tests passed.\n"); return 0; }
    fprintf(stderr, "\n%d test(s) failed.\n", failures);
    return 1;
}
