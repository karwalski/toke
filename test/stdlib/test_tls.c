/*
 * test_tls.c — Unit tests for the std.tls C library.
 *
 * Build:
 *   cc -Wall -Wextra -Isrc -Isrc/stdlib \
 *      src/stdlib/tls.c test/stdlib/test_tls.c \
 *      -I/opt/homebrew/include -L/opt/homebrew/lib \
 *      -lssl -lcrypto -lpthread \
 *      -o /tmp/test_tls && /tmp/test_tls
 *
 * Test inventory (22 tests):
 *  1.  gen_self_signed: succeeds with valid inputs
 *  2.  gen_self_signed: cert_pem begins with "-----BEGIN CERTIFICATE-----"
 *  3.  gen_self_signed: key_pem begins with "-----BEGIN"
 *  4.  gen_self_signed: fails on empty common_name
 *  5.  gen_self_signed: fails on zero valid_days
 *  6.  gen_self_signed: fails on negative valid_days
 *  7.  fingerprint: returns 64-char hex string for valid PEM
 *  8.  fingerprint: all characters are valid hex digits
 *  9.  fingerprint: same PEM yields same fingerprint (deterministic)
 * 10.  fingerprint: different certs yield different fingerprints
 * 11.  fingerprint: empty PEM returns empty string
 * 12.  fingerprint: junk PEM returns empty string
 * 13.  tls_conn_free_id: does not crash on heap-allocated id
 * 14.  tls_connect: returns is_none=1 on refused port
 * 15.  tls_connect: returns is_none=1 on unresolvable host
 * 16.  tls_read: returns is_none=1 on null conn id
 * 17.  tls_write: returns 0 on null conn id
 * 18.  tls_close: returns 0 on null conn id
 * 19.  tls_peer_cert: returns is_none=1 on null conn id
 * 20.  tls_pairing_code: returns "000000" on null conn id
 * 21.  tls_pairing_code: returns 6-digit string
 * 22.  gen_self_signed + fingerprint: round-trip consistency
 *
 * Story: 72.9
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#include "../../src/stdlib/tls.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL: %s\n", (msg)); failures++; } \
        else         { printf("pass: %s\n",           (msg)); } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a), (b)) == 0, (msg))

#define ASSERT_STRPFX(s, pfx, msg) \
    ASSERT((s) && strncmp((s), (pfx), strlen(pfx)) == 0, (msg))

/* -------------------------------------------------------------------------
 * Helper: is_hex_string — check all chars in s[0..len-1] are hex digits
 * ------------------------------------------------------------------------- */
static int is_hex_string(const char *s, size_t len)
{
    if (!s) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

/* =========================================================================
 * Tests 1-6: gen_self_signed
 * ========================================================================= */

static void test_gen_self_signed_success(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("test.local", 365);
    ASSERT(!kp.is_err, "gen_self_signed: is_err == 0 for valid input");
    ASSERT(kp.cert_pem != NULL, "gen_self_signed: cert_pem != NULL");
    ASSERT(kp.key_pem  != NULL, "gen_self_signed: key_pem  != NULL");
    free(kp.cert_pem);
    free(kp.key_pem);
}

static void test_gen_self_signed_pem_prefix(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("pem.prefix.test", 90);
    ASSERT(!kp.is_err, "gen_self_signed pem_prefix: no error");
    ASSERT_STRPFX(kp.cert_pem, "-----BEGIN CERTIFICATE-----",
                  "gen_self_signed: cert_pem has correct PEM header");
    ASSERT_STRPFX(kp.key_pem,  "-----BEGIN",
                  "gen_self_signed: key_pem has PEM header");
    free(kp.cert_pem);
    free(kp.key_pem);
}

static void test_gen_self_signed_fail_empty_cn(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("", 365);
    ASSERT(kp.is_err, "gen_self_signed: is_err == 1 for empty common_name");
}

static void test_gen_self_signed_fail_null_cn(void)
{
    TlsKeypairResult kp = tls_gen_self_signed(NULL, 365);
    ASSERT(kp.is_err, "gen_self_signed: is_err == 1 for NULL common_name");
}

static void test_gen_self_signed_fail_zero_days(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("zero.days", 0);
    ASSERT(kp.is_err, "gen_self_signed: is_err == 1 for valid_days == 0");
}

static void test_gen_self_signed_fail_negative_days(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("neg.days", -10);
    ASSERT(kp.is_err, "gen_self_signed: is_err == 1 for negative valid_days");
}

/* =========================================================================
 * Tests 7-12: fingerprint
 * ========================================================================= */

static void test_fingerprint_length(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("fp.len", 30);
    ASSERT(!kp.is_err, "fingerprint_length: gen ok");
    if (kp.is_err) { return; }

    char *fp = tls_fingerprint(kp.cert_pem);
    ASSERT(fp != NULL, "fingerprint: not NULL");
    ASSERT(strlen(fp) == 64, "fingerprint: length == 64");

    free(fp);
    free(kp.cert_pem);
    free(kp.key_pem);
}

static void test_fingerprint_hex_chars(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("fp.hex", 30);
    ASSERT(!kp.is_err, "fingerprint_hex: gen ok");
    if (kp.is_err) { return; }

    char *fp = tls_fingerprint(kp.cert_pem);
    ASSERT(fp && is_hex_string(fp, 64), "fingerprint: all hex digits");

    free(fp);
    free(kp.cert_pem);
    free(kp.key_pem);
}

static void test_fingerprint_deterministic(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("fp.det", 30);
    ASSERT(!kp.is_err, "fingerprint_det: gen ok");
    if (kp.is_err) { return; }

    char *fp1 = tls_fingerprint(kp.cert_pem);
    char *fp2 = tls_fingerprint(kp.cert_pem);
    ASSERT(fp1 && fp2 && strcmp(fp1, fp2) == 0,
           "fingerprint: same PEM yields same fingerprint");

    free(fp1);
    free(fp2);
    free(kp.cert_pem);
    free(kp.key_pem);
}

static void test_fingerprint_different_certs(void)
{
    TlsKeypairResult kp1 = tls_gen_self_signed("fp.diff.1", 30);
    TlsKeypairResult kp2 = tls_gen_self_signed("fp.diff.2", 30);
    ASSERT(!kp1.is_err && !kp2.is_err, "fingerprint_diff: both gen ok");
    if (kp1.is_err || kp2.is_err) {
        free(kp1.cert_pem); free(kp1.key_pem);
        free(kp2.cert_pem); free(kp2.key_pem);
        return;
    }

    char *fp1 = tls_fingerprint(kp1.cert_pem);
    char *fp2 = tls_fingerprint(kp2.cert_pem);
    ASSERT(fp1 && fp2 && strcmp(fp1, fp2) != 0,
           "fingerprint: different certs yield different fingerprints");

    free(fp1); free(fp2);
    free(kp1.cert_pem); free(kp1.key_pem);
    free(kp2.cert_pem); free(kp2.key_pem);
}

static void test_fingerprint_empty_pem(void)
{
    char *fp = tls_fingerprint("");
    ASSERT(fp != NULL, "fingerprint empty: returns non-NULL");
    ASSERT(strlen(fp) == 0, "fingerprint empty: returns empty string");
    free(fp);
}

static void test_fingerprint_junk_pem(void)
{
    char *fp = tls_fingerprint("THIS IS NOT A VALID PEM STRING AT ALL");
    ASSERT(fp != NULL, "fingerprint junk: returns non-NULL");
    ASSERT(strlen(fp) == 0, "fingerprint junk: returns empty string");
    free(fp);
}

/* =========================================================================
 * Test 13: tls_conn_free_id
 * ========================================================================= */

static void test_conn_free_id(void)
{
    TlsConn conn;
    conn.id = strdup("42:0x0000");
    tls_conn_free_id(conn);
    /* If we reach here without crashing, the test passes */
    ASSERT(1, "tls_conn_free_id: does not crash");
}

/* =========================================================================
 * Tests 14-15: tls_connect with no server
 * ========================================================================= */

static void test_connect_refused(void)
{
    TlsConfig cfg = {NULL, NULL, NULL, 0};
    /* Port 1 is almost certainly refused on any dev machine */
    TlsConnResult r = tls_connect("127.0.0.1", 1, cfg);
    ASSERT(r.is_none == 1, "tls_connect: is_none==1 for refused connection");
    free(r.conn.id);
}

static void test_connect_bad_host(void)
{
    TlsConfig cfg = {NULL, NULL, NULL, 0};
    TlsConnResult r = tls_connect("this.host.does.not.exist.invalid", 8443, cfg);
    ASSERT(r.is_none == 1, "tls_connect: is_none==1 for unresolvable host");
    free(r.conn.id);
}

/* =========================================================================
 * Tests 16-20: null-conn safety
 * ========================================================================= */

static void test_read_null_conn(void)
{
    TlsConn conn = {NULL};
    TlsStrResult r = tls_read(conn);
    ASSERT(r.is_none == 1, "tls_read: is_none==1 for null conn.id");
}

static void test_write_null_conn(void)
{
    TlsConn conn = {NULL};
    int ok = tls_write(conn, "hello");
    ASSERT(ok == 0, "tls_write: returns 0 for null conn.id");
}

static void test_close_null_conn(void)
{
    TlsConn conn = {NULL};
    int ok = tls_close(conn);
    ASSERT(ok == 0, "tls_close: returns 0 for null conn.id");
}

static void test_peer_cert_null_conn(void)
{
    TlsConn conn = {NULL};
    TlsStrResult r = tls_peer_cert(conn);
    ASSERT(r.is_none == 1, "tls_peer_cert: is_none==1 for null conn.id");
}

static void test_pairing_code_null_conn(void)
{
    TlsConn conn = {NULL};
    char *code = tls_pairing_code(conn);
    ASSERT_STREQ(code, "000000", "tls_pairing_code: returns '000000' for null conn.id");
    free(code);
}

/* =========================================================================
 * Test 21: pairing code format
 * ========================================================================= */

static void test_pairing_code_format(void)
{
    /* We can't get a real conn without a live server, so test the format via
     * the fallback path and verify 6-digit constraint on the output. */
    TlsConn conn = {NULL};
    char *code = tls_pairing_code(conn);
    ASSERT(code != NULL, "tls_pairing_code: not NULL");
    ASSERT(strlen(code) == 6, "tls_pairing_code: exactly 6 chars");
    int all_digits = 1;
    for (int i = 0; i < 6; i++) {
        if (!isdigit((unsigned char)code[i])) { all_digits = 0; break; }
    }
    ASSERT(all_digits, "tls_pairing_code: all digits");
    free(code);
}

/* =========================================================================
 * Test 22: gen_self_signed + fingerprint round-trip
 * ========================================================================= */

static void test_gen_and_fingerprint_roundtrip(void)
{
    TlsKeypairResult kp = tls_gen_self_signed("roundtrip.test", 1);
    ASSERT(!kp.is_err, "roundtrip: gen_self_signed succeeds");
    if (kp.is_err) return;

    char *fp = tls_fingerprint(kp.cert_pem);
    ASSERT(fp && strlen(fp) == 64,
           "roundtrip: fingerprint of generated cert is 64 chars");
    ASSERT(fp && is_hex_string(fp, 64),
           "roundtrip: fingerprint of generated cert is hex");

    free(fp);
    free(kp.cert_pem);
    free(kp.key_pem);
}

/* =========================================================================
 * main
 * ========================================================================= */

int main(void)
{
    /* gen_self_signed */
    test_gen_self_signed_success();
    test_gen_self_signed_pem_prefix();
    test_gen_self_signed_fail_empty_cn();
    test_gen_self_signed_fail_null_cn();
    test_gen_self_signed_fail_zero_days();
    test_gen_self_signed_fail_negative_days();

    /* fingerprint */
    test_fingerprint_length();
    test_fingerprint_hex_chars();
    test_fingerprint_deterministic();
    test_fingerprint_different_certs();
    test_fingerprint_empty_pem();
    test_fingerprint_junk_pem();

    /* conn_free_id */
    test_conn_free_id();

    /* connect */
    test_connect_refused();
    test_connect_bad_host();

    /* null-conn safety */
    test_read_null_conn();
    test_write_null_conn();
    test_close_null_conn();
    test_peer_cert_null_conn();
    test_pairing_code_null_conn();

    /* pairing code format */
    test_pairing_code_format();

    /* round-trip */
    test_gen_and_fingerprint_roundtrip();

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
