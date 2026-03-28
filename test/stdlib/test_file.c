/*
 * test_file.c — Unit tests for the std.file C library (Story 1.3.5).
 *
 * Build and run: make test-stdlib
 */

#include <stdio.h>
#include <string.h>
#include "../../src/stdlib/file.h"

#define TEST_PATH "/tmp/toke_test_file.txt"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)
#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* Ensure clean state */
    file_delete(TEST_PATH);

    /* file_write */
    BoolFileResult wr = file_write(TEST_PATH, "hello world");
    ASSERT(!wr.is_err && wr.ok, "write returns true");

    /* file_read */
    StrFileResult rd = file_read(TEST_PATH);
    ASSERT(!rd.is_err, "read ok after write");
    ASSERT_STREQ(rd.ok, "hello world", "read content == 'hello world'");

    /* file_exists — present */
    ASSERT(file_exists(TEST_PATH), "exists == true after write");

    /* file_append */
    BoolFileResult ap = file_append(TEST_PATH, " extra");
    ASSERT(!ap.is_err && ap.ok, "append returns true");

    /* file_read after append */
    StrFileResult rd2 = file_read(TEST_PATH);
    ASSERT(!rd2.is_err, "read ok after append");
    ASSERT_STREQ(rd2.ok, "hello world extra", "read content == 'hello world extra'");

    /* file_delete */
    BoolFileResult del = file_delete(TEST_PATH);
    ASSERT(!del.is_err && del.ok, "delete returns true");

    /* file_exists — absent */
    ASSERT(!file_exists(TEST_PATH), "exists == false after delete");

    /* file_read non-existent → FILE_ERR_NOT_FOUND */
    StrFileResult rd3 = file_read(TEST_PATH);
    ASSERT(rd3.is_err && rd3.err.kind == FILE_ERR_NOT_FOUND,
           "read missing file → FILE_ERR_NOT_FOUND");

    /* file_list /tmp → at least one entry */
    StrArrayFileResult ls = file_list("/tmp");
    ASSERT(!ls.is_err && ls.ok.len >= 1, "list /tmp returns >= 1 entry");

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
