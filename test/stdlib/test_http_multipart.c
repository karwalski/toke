/*
 * test_http_multipart.c — Unit tests for multipart/form-data parsing
 * (Story 27.1.8).
 *
 * Build:
 *   cc -std=c99 -Wall -Wextra -Wpedantic -Werror \
 *      -o test/stdlib/test_http_multipart \
 *      test/stdlib/test_http_multipart.c src/stdlib/http.c
 * Or via make:
 *   make test-stdlib-http-multipart
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

#define ASSERT_SIZE(a, b, msg) \
    ASSERT((a) == (b), (msg))

/* sfree — cast away const for free() */
static void sfree(const char *p) { free((void *)(uintptr_t)p); }

/* ── Helper: build a well-formed multipart body ─────────────────────── */

/*
 * build_body — assemble a raw multipart body from N (headers, data) pairs.
 * The caller provides an array of header strings and data strings.
 * Returns heap-allocated body and sets *len_out.
 */
static char *build_body(const char *boundary,
                         const char * const *part_hdrs,
                         const char * const *part_data,
                         size_t nparts,
                         size_t *len_out)
{
    /* First pass: calculate required size */
    size_t delim_len = strlen(boundary) + 2; /* "--" + boundary */
    size_t total = 0;
    for (size_t i = 0; i < nparts; i++) {
        total += delim_len + 2;                  /* --boundary\r\n */
        total += strlen(part_hdrs[i]);           /* headers */
        total += 4;                              /* \r\n\r\n */
        total += strlen(part_data[i]);           /* data */
        total += 2;                              /* \r\n before next delim */
    }
    total += delim_len + 2 + 2;                 /* --boundary--\r\n */
    total += 1;                                 /* NUL */

    char *buf = malloc(total);
    if (!buf) return NULL;

    char *p = buf;
    for (size_t i = 0; i < nparts; i++) {
        p += sprintf(p, "--%s\r\n", boundary);
        p += sprintf(p, "%s", part_hdrs[i]);
        p += sprintf(p, "\r\n\r\n");
        size_t dlen = strlen(part_data[i]);
        memcpy(p, part_data[i], dlen);
        p += dlen;
        memcpy(p, "\r\n", 2);
        p += 2;
    }
    p += sprintf(p, "--%s--\r\n", boundary);

    *len_out = (size_t)(p - buf);
    return buf;
}

/* ── http_multipart_boundary tests ──────────────────────────────────── */

static void test_boundary_simple(void)
{
    const char *ct  = "multipart/form-data; boundary=abc123";
    const char *bnd = http_multipart_boundary(ct);
    ASSERT_STREQ(bnd, "abc123", "boundary: simple unquoted");
    sfree(bnd);
}

static void test_boundary_webkit(void)
{
    const char *ct  = "multipart/form-data; boundary=----WebKitFormBoundaryXYZ";
    const char *bnd = http_multipart_boundary(ct);
    ASSERT_STREQ(bnd, "----WebKitFormBoundaryXYZ", "boundary: WebKit-style");
    sfree(bnd);
}

static void test_boundary_quoted(void)
{
    const char *ct  = "multipart/form-data; boundary=\"my boundary\"";
    const char *bnd = http_multipart_boundary(ct);
    ASSERT_STREQ(bnd, "my boundary", "boundary: quoted value");
    sfree(bnd);
}

static void test_boundary_null_ct(void)
{
    const char *bnd = http_multipart_boundary(NULL);
    ASSERT_NULL(bnd, "boundary: NULL content-type returns NULL");
}

static void test_boundary_no_boundary(void)
{
    const char *bnd = http_multipart_boundary("application/json");
    ASSERT_NULL(bnd, "boundary: no boundary param returns NULL");
}

/* ── http_multipart_parse: simple text field ─────────────────────────── */

static void test_simple_text_field(void)
{
    const char *bnd = "boundary42";
    const char *hdrs[1] = {
        "Content-Disposition: form-data; name=\"username\""
    };
    const char *data[1] = { "alice" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 1, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 1U, "simple text field: 1 part");
    ASSERT_STREQ(r.parts[0].name, "username", "simple text field: name");
    ASSERT_NULL(r.parts[0].filename, "simple text field: no filename");
    ASSERT_NULL(r.parts[0].content_type, "simple text field: no content-type");
    ASSERT_STREQ(r.parts[0].data, "alice", "simple text field: data");
    ASSERT_SIZE(r.parts[0].data_len, 5U, "simple text field: data_len");

    http_multipart_free(r);
    free(body);
}

/* ── File upload with filename ───────────────────────────────────────── */

static void test_file_upload(void)
{
    const char *bnd = "uploadBoundary";
    const char *hdrs[1] = {
        "Content-Disposition: form-data; name=\"file\"; filename=\"hello.txt\"\r\n"
        "Content-Type: text/plain"
    };
    const char *data[1] = { "Hello, world!" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 1, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 1U, "file upload: 1 part");
    ASSERT_STREQ(r.parts[0].name,         "file",       "file upload: name");
    ASSERT_STREQ(r.parts[0].filename,     "hello.txt",  "file upload: filename");
    ASSERT_STREQ(r.parts[0].content_type, "text/plain", "file upload: content-type");
    ASSERT_STREQ(r.parts[0].data,         "Hello, world!", "file upload: data");

    http_multipart_free(r);
    free(body);
}

/* ── Multiple parts ──────────────────────────────────────────────────── */

static void test_multiple_parts(void)
{
    const char *bnd = "multiBnd";
    const char *hdrs[3] = {
        "Content-Disposition: form-data; name=\"first\"",
        "Content-Disposition: form-data; name=\"second\"",
        "Content-Disposition: form-data; name=\"third\""
    };
    const char *data[3] = { "one", "two", "three" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 3, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 3U, "multiple parts: 3 parts");
    ASSERT_STREQ(r.parts[0].name, "first",  "multiple parts: name[0]");
    ASSERT_STREQ(r.parts[1].name, "second", "multiple parts: name[1]");
    ASSERT_STREQ(r.parts[2].name, "third",  "multiple parts: name[2]");
    ASSERT_STREQ(r.parts[0].data, "one",    "multiple parts: data[0]");
    ASSERT_STREQ(r.parts[1].data, "two",    "multiple parts: data[1]");
    ASSERT_STREQ(r.parts[2].data, "three",  "multiple parts: data[2]");

    http_multipart_free(r);
    free(body);
}

/* ── Part with no filename (regular field) ───────────────────────────── */

static void test_regular_field_no_filename(void)
{
    const char *bnd = "bnd";
    const char *hdrs[1] = {
        "Content-Disposition: form-data; name=\"email\""
    };
    const char *data[1] = { "user@example.com" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 1, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 1U, "regular field: 1 part");
    ASSERT_NULL(r.parts[0].filename, "regular field: filename is NULL");
    ASSERT_STREQ(r.parts[0].data, "user@example.com", "regular field: data");

    http_multipart_free(r);
    free(body);
}

/* ── Empty field value ───────────────────────────────────────────────── */

static void test_empty_field_value(void)
{
    const char *bnd = "emptyBnd";
    const char *hdrs[1] = {
        "Content-Disposition: form-data; name=\"empty\""
    };
    const char *data[1] = { "" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 1, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 1U, "empty field: 1 part");
    ASSERT_SIZE(r.parts[0].data_len, 0U, "empty field: data_len == 0");

    http_multipart_free(r);
    free(body);
}

/* ── Field with binary data ──────────────────────────────────────────── */

static void test_binary_data(void)
{
    const char *bnd = "binBnd";
    /* build the body manually so we can embed NUL bytes */
    const char prefix[]  = "--binBnd\r\n"
                            "Content-Disposition: form-data; name=\"blob\"; filename=\"data.bin\"\r\n"
                            "Content-Type: application/octet-stream"
                            "\r\n\r\n";
    /* 8-byte payload with embedded NUL at position 3 */
    const char payload[] = "\x01\x02\x03\x00\x05\x06\x07\x08";
    const char suffix[]  = "\r\n--binBnd--\r\n";

    size_t pre_len  = sizeof(prefix) - 1;
    size_t pay_len  = 8;
    size_t suf_len  = sizeof(suffix) - 1;
    size_t body_len = pre_len + pay_len + suf_len;

    char *body = malloc(body_len + 1);
    if (!body) { ASSERT(0, "binary data: alloc failed"); return; }
    memcpy(body,             prefix,  pre_len);
    memcpy(body + pre_len,  payload, pay_len);
    memcpy(body + pre_len + pay_len, suffix, suf_len);
    body[body_len] = '\0';

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 1U, "binary data: 1 part");
    ASSERT_SIZE(r.parts[0].data_len, 8U, "binary data: data_len == 8");
    ASSERT(memcmp(r.parts[0].data, payload, 8) == 0,
           "binary data: payload matches");

    http_multipart_free(r);
    free(body);
}

/* ── Missing boundary returns empty result ───────────────────────────── */

static void test_missing_boundary_returns_empty(void)
{
    const char body[] = "--bnd\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nval\r\n--bnd--\r\n";
    TkMultipartResult r = http_multipart_parse(NULL, body, sizeof(body) - 1);
    ASSERT_SIZE(r.nparts, 0U, "NULL boundary: empty result");
    http_multipart_free(r);
}

/* ── Empty boundary string returns empty result ──────────────────────── */

static void test_empty_boundary_returns_empty(void)
{
    const char body[] = "--bnd\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nval\r\n--bnd--\r\n";
    TkMultipartResult r = http_multipart_parse("", body, sizeof(body) - 1);
    ASSERT_SIZE(r.nparts, 0U, "empty boundary: empty result");
    http_multipart_free(r);
}

/* ── Wrong boundary returns empty result ─────────────────────────────── */

static void test_wrong_boundary_returns_empty(void)
{
    const char *bnd = "correct";
    const char *hdrs[1] = {
        "Content-Disposition: form-data; name=\"f\""
    };
    const char *data[1] = { "v" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 1, &body_len);

    TkMultipartResult r = http_multipart_parse("wrong", body, body_len);
    ASSERT_SIZE(r.nparts, 0U, "wrong boundary: empty result");

    http_multipart_free(r);
    free(body);
}

/* ── Boundary extraction from full Content-Type header ───────────────── */

static void test_boundary_from_content_type_with_prefix(void)
{
    const char *ct  = "multipart/form-data; boundary=----FormBoundary7MA4YWxkTrZu0gW";
    const char *bnd = http_multipart_boundary(ct);
    ASSERT_STREQ(bnd, "----FormBoundary7MA4YWxkTrZu0gW",
                 "boundary extraction: long form-data boundary");
    sfree(bnd);
}

/* ── Mixed: text field + file upload ─────────────────────────────────── */

static void test_mixed_text_and_file(void)
{
    const char *bnd = "mixedBnd";
    const char *hdrs[2] = {
        "Content-Disposition: form-data; name=\"description\"",
        "Content-Disposition: form-data; name=\"upload\"; filename=\"photo.jpg\"\r\n"
        "Content-Type: image/jpeg"
    };
    const char *data[2] = { "A nice photo", "\xff\xd8\xff" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 2, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 2U, "mixed: 2 parts");
    ASSERT_STREQ(r.parts[0].name, "description",  "mixed: part[0] name");
    ASSERT_NULL(r.parts[0].filename,               "mixed: part[0] no filename");
    ASSERT_STREQ(r.parts[1].name, "upload",        "mixed: part[1] name");
    ASSERT_STREQ(r.parts[1].filename, "photo.jpg", "mixed: part[1] filename");
    ASSERT_STREQ(r.parts[1].content_type, "image/jpeg",
                 "mixed: part[1] content-type");

    http_multipart_free(r);
    free(body);
}

/* ── NULL body returns empty result ──────────────────────────────────── */

static void test_null_body_returns_empty(void)
{
    TkMultipartResult r = http_multipart_parse("bnd", NULL, 10);
    ASSERT_SIZE(r.nparts, 0U, "NULL body: empty result");
    http_multipart_free(r);
}

/* ── Zero body length returns empty result ───────────────────────────── */

static void test_zero_body_len_returns_empty(void)
{
    const char body[] = "--bnd\r\nContent-Disposition: form-data; name=\"x\"\r\n\r\nv\r\n--bnd--\r\n";
    TkMultipartResult r = http_multipart_parse("bnd", body, 0);
    ASSERT_SIZE(r.nparts, 0U, "zero body_len: empty result");
    http_multipart_free(r);
}

/* ── Boundary extraction: case-insensitive BOUNDARY= ─────────────────── */

static void test_boundary_case_insensitive(void)
{
    const char *ct  = "multipart/form-data; BOUNDARY=caseTest";
    const char *bnd = http_multipart_boundary(ct);
    ASSERT_STREQ(bnd, "caseTest", "boundary: case-insensitive BOUNDARY=");
    sfree(bnd);
}

/* ── Multiple fields with identical names ────────────────────────────── */

static void test_multiple_same_name_fields(void)
{
    const char *bnd = "sameBnd";
    const char *hdrs[3] = {
        "Content-Disposition: form-data; name=\"color\"",
        "Content-Disposition: form-data; name=\"color\"",
        "Content-Disposition: form-data; name=\"color\""
    };
    const char *data[3] = { "red", "green", "blue" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 3, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 3U, "same-name fields: 3 parts");
    ASSERT_STREQ(r.parts[0].data, "red",   "same-name fields: data[0]");
    ASSERT_STREQ(r.parts[1].data, "green", "same-name fields: data[1]");
    ASSERT_STREQ(r.parts[2].data, "blue",  "same-name fields: data[2]");

    http_multipart_free(r);
    free(body);
}

/* ── File upload with spaces in filename ─────────────────────────────── */

static void test_filename_with_spaces(void)
{
    const char *bnd = "spacesBnd";
    const char *hdrs[1] = {
        "Content-Disposition: form-data; name=\"doc\"; filename=\"my document.pdf\"\r\n"
        "Content-Type: application/pdf"
    };
    const char *data[1] = { "%PDF-1.4" };
    size_t body_len = 0;
    char  *body     = build_body(bnd, hdrs, data, 1, &body_len);

    TkMultipartResult r = http_multipart_parse(bnd, body, body_len);
    ASSERT_SIZE(r.nparts, 1U, "filename with spaces: 1 part");
    ASSERT_STREQ(r.parts[0].filename, "my document.pdf",
                 "filename with spaces: filename preserved");

    http_multipart_free(r);
    free(body);
}

/* ── main ──────────────────────────────────────────────────────────── */

int main(void)
{
    /* http_multipart_boundary tests */
    test_boundary_simple();
    test_boundary_webkit();
    test_boundary_quoted();
    test_boundary_null_ct();
    test_boundary_no_boundary();
    test_boundary_from_content_type_with_prefix();
    test_boundary_case_insensitive();

    /* http_multipart_parse tests */
    test_simple_text_field();
    test_file_upload();
    test_multiple_parts();
    test_regular_field_no_filename();
    test_empty_field_value();
    test_binary_data();
    test_missing_boundary_returns_empty();
    test_empty_boundary_returns_empty();
    test_wrong_boundary_returns_empty();
    test_mixed_text_and_file();
    test_null_body_returns_empty();
    test_zero_body_len_returns_empty();
    test_multiple_same_name_fields();
    test_filename_with_spaces();

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
