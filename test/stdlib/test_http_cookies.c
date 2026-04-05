/*
 * test_http_cookies.c — Unit tests for cookie parsing and Set-Cookie
 * header building (Story 27.1.7).
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *      -o test/stdlib/test_http_cookies \
 *      test/stdlib/test_http_cookies.c src/stdlib/http.c
 * Or via make:
 *   make test-stdlib-http-cookies
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "../../src/stdlib/http.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { \
            fprintf(stderr, "FAIL [%d]: %s\n", __LINE__, (msg)); \
            failures++; \
        } else { \
            printf("pass: %s\n", (msg)); \
        } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a), (b)) == 0, (msg))

#define ASSERT_NULL(a, msg) \
    ASSERT((a) == NULL, (msg))

/* helper — free and silence the const warning */
static void sfree(const char *p) { free((void *)(uintptr_t)p); }

/* ── http_cookie tests ─────────────────────────────────────────────── */

static void test_single_cookie(void)
{
    const char *hdr = "session=abc123";
    const char *v = http_cookie(hdr, "session");
    ASSERT_STREQ(v, "abc123", "single cookie: correct value");
    sfree(v);
}

static void test_multiple_cookies_first(void)
{
    const char *hdr = "a=1; b=2; c=3";
    const char *v = http_cookie(hdr, "a");
    ASSERT_STREQ(v, "1", "multiple cookies: find first");
    sfree(v);
}

static void test_multiple_cookies_middle(void)
{
    const char *hdr = "a=1; b=hello; c=3";
    const char *v = http_cookie(hdr, "b");
    ASSERT_STREQ(v, "hello", "multiple cookies: find middle");
    sfree(v);
}

static void test_multiple_cookies_last(void)
{
    const char *hdr = "a=1; b=2; token=xyz";
    const char *v = http_cookie(hdr, "token");
    ASSERT_STREQ(v, "xyz", "multiple cookies: find last");
    sfree(v);
}

static void test_missing_cookie(void)
{
    const char *hdr = "a=1; b=2";
    const char *v = http_cookie(hdr, "z");
    ASSERT_NULL(v, "missing cookie returns NULL");
}

static void test_null_header(void)
{
    const char *v = http_cookie(NULL, "a");
    ASSERT_NULL(v, "NULL header returns NULL");
}

static void test_null_name(void)
{
    const char *v = http_cookie("a=1", NULL);
    ASSERT_NULL(v, "NULL name returns NULL");
}

static void test_url_decoded_value(void)
{
    /* %20 = space */
    const char *hdr = "greeting=Hello%20World";
    const char *v = http_cookie(hdr, "greeting");
    ASSERT_STREQ(v, "Hello World", "URL-decoded value (space)");
    sfree(v);
}

static void test_url_decoded_hex_lower(void)
{
    /* %2f = '/' */
    const char *hdr = "path=%2fhome%2fuser";
    const char *v = http_cookie(hdr, "path");
    ASSERT_STREQ(v, "/home/user", "URL-decoded value (lower-hex slashes)");
    sfree(v);
}

static void test_cookie_with_equals_in_value(void)
{
    /* Base64 values contain '='; the first '=' is the name separator,
     * remainder belongs to the value. */
    const char *hdr = "tok=dGVzdA==; other=x";
    const char *v = http_cookie(hdr, "tok");
    ASSERT_STREQ(v, "dGVzdA==", "cookie value containing '='");
    sfree(v);
}

static void test_whitespace_around_semicolon(void)
{
    const char *hdr = "  x = 10 ;  y = 20 ";
    const char *v = http_cookie(hdr, "x");
    ASSERT_STREQ(v, "10", "whitespace trimmed around name+value");
    sfree(v);
    const char *v2 = http_cookie(hdr, "y");
    ASSERT_STREQ(v2, "20", "whitespace trimmed from final value");
    sfree(v2);
}

/* ── http_set_cookie_header tests ──────────────────────────────────── */

static void test_set_cookie_minimal(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_age = -1; /* omit */
    const char *h = http_set_cookie_header("sid", "abc", opts);
    ASSERT_STREQ(h, "sid=abc", "set-cookie minimal (name=value only)");
    sfree(h);
}

static void test_set_cookie_with_path(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.path    = "/";
    opts.max_age = -1;
    const char *h = http_set_cookie_header("x", "1", opts);
    ASSERT_STREQ(h, "x=1; Path=/", "set-cookie with Path=/");
    sfree(h);
}

static void test_set_cookie_with_domain(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.domain  = "example.com";
    opts.max_age = -1;
    const char *h = http_set_cookie_header("d", "v", opts);
    ASSERT_STREQ(h, "d=v; Domain=example.com", "set-cookie with Domain");
    sfree(h);
}

static void test_set_cookie_secure_httponly(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_age   = -1;
    opts.secure    = 1;
    opts.http_only = 1;
    const char *h = http_set_cookie_header("s", "v", opts);
    ASSERT_STREQ(h, "s=v; Secure; HttpOnly", "set-cookie Secure+HttpOnly");
    sfree(h);
}

static void test_set_cookie_max_age_zero(void)
{
    /* max_age=0 is the standard way to delete a cookie */
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.path    = "/";
    opts.max_age = 0;
    const char *h = http_set_cookie_header("gone", "", opts);
    ASSERT_STREQ(h, "gone=; Path=/; Max-Age=0", "set-cookie max_age=0 deletes cookie");
    sfree(h);
}

static void test_set_cookie_samesite_strict(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_age   = -1;
    opts.same_site = "Strict";
    const char *h = http_set_cookie_header("c", "v", opts);
    ASSERT_STREQ(h, "c=v; SameSite=Strict", "set-cookie SameSite=Strict");
    sfree(h);
}

static void test_set_cookie_samesite_lax(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_age   = -1;
    opts.same_site = "Lax";
    const char *h = http_set_cookie_header("c", "v", opts);
    ASSERT_STREQ(h, "c=v; SameSite=Lax", "set-cookie SameSite=Lax");
    sfree(h);
}

static void test_set_cookie_samesite_none(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_age   = -1;
    opts.secure    = 1; /* SameSite=None requires Secure */
    opts.same_site = "None";
    const char *h = http_set_cookie_header("c", "v", opts);
    ASSERT_STREQ(h, "c=v; Secure; SameSite=None", "set-cookie SameSite=None+Secure");
    sfree(h);
}

static void test_set_cookie_all_options(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.path      = "/app";
    opts.domain    = "example.com";
    opts.max_age   = 3600;
    opts.secure    = 1;
    opts.http_only = 1;
    opts.same_site = "Lax";
    const char *h = http_set_cookie_header("full", "val", opts);
    ASSERT_STREQ(h,
        "full=val; Path=/app; Domain=example.com; Max-Age=3600; "
        "Secure; HttpOnly; SameSite=Lax",
        "set-cookie all options");
    sfree(h);
}

static void test_set_cookie_null_name_returns_null(void)
{
    TkCookieOpts opts;
    memset(&opts, 0, sizeof(opts));
    opts.max_age = -1;
    const char *h = http_set_cookie_header(NULL, "v", opts);
    ASSERT_NULL(h, "set-cookie NULL name returns NULL");
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
    /* http_cookie tests */
    test_single_cookie();
    test_multiple_cookies_first();
    test_multiple_cookies_middle();
    test_multiple_cookies_last();
    test_missing_cookie();
    test_null_header();
    test_null_name();
    test_url_decoded_value();
    test_url_decoded_hex_lower();
    test_cookie_with_equals_in_value();
    test_whitespace_around_semicolon();

    /* http_set_cookie_header tests */
    test_set_cookie_minimal();
    test_set_cookie_with_path();
    test_set_cookie_with_domain();
    test_set_cookie_secure_httponly();
    test_set_cookie_max_age_zero();
    test_set_cookie_samesite_strict();
    test_set_cookie_samesite_lax();
    test_set_cookie_samesite_none();
    test_set_cookie_all_options();
    test_set_cookie_null_name_returns_null();

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
