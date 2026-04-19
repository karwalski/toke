/*
 * test_keychain.c — Unit tests for the std.keychain C library.
 *
 * Build and run (macOS):
 *   cc -Wall -Wextra -Isrc -Isrc/stdlib \
 *       test/stdlib/test_keychain.c src/stdlib/keychain.c \
 *       -framework Security -o /tmp/test_keychain && /tmp/test_keychain
 *
 * Story: 72.1
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/stdlib/keychain.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
        else          { printf("pass: %s\n", msg); } \
    } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a), (b)) == 0, msg)

/* Unique service name per test run to avoid collisions with real entries. */
#define TEST_SERVICE  "com.toke.test.keychain"
#define TEST_ACCOUNT  "test_account"
#define TEST_SECRET   "t3st-s3cr3t-v4lu3"
#define TEST_SECRET2  "updated-s3cr3t"

static void test_is_available(void)
{
    int avail = keychain_is_available();
    /* On macOS/Windows this should be 1; on other platforms 0 is correct. */
    /* We just verify it returns a valid boolean (0 or 1). */
    ASSERT(avail == 0 || avail == 1, "is_available() returns 0 or 1");
}

static void test_null_args(void)
{
    ASSERT(keychain_set(NULL, TEST_ACCOUNT, TEST_SECRET) == 0,
           "set(NULL service) returns 0");
    ASSERT(keychain_set(TEST_SERVICE, NULL, TEST_SECRET) == 0,
           "set(NULL account) returns 0");
    ASSERT(keychain_set(TEST_SERVICE, TEST_ACCOUNT, NULL) == 0,
           "set(NULL secret) returns 0");
    ASSERT(keychain_set(TEST_SERVICE, TEST_ACCOUNT, "") == 0,
           "set(empty secret) returns 0");

    ASSERT(keychain_get(NULL, TEST_ACCOUNT) == NULL,
           "get(NULL service) returns NULL");
    ASSERT(keychain_get(TEST_SERVICE, NULL) == NULL,
           "get(NULL account) returns NULL");

    ASSERT(keychain_delete(NULL, TEST_ACCOUNT) == 0,
           "delete(NULL service) returns 0");
    ASSERT(keychain_delete(TEST_SERVICE, NULL) == 0,
           "delete(NULL account) returns 0");

    ASSERT(keychain_exists(NULL, TEST_ACCOUNT) == 0,
           "exists(NULL service) returns 0");
    ASSERT(keychain_exists(TEST_SERVICE, NULL) == 0,
           "exists(NULL account) returns 0");
}

/*
 * Functional tests — only run when the keychain is actually available,
 * so the test suite remains green in CI environments without a keychain.
 */
static void test_functional(void)
{
    /* Clean up any leftover entry from a previous test run. */
    keychain_delete(TEST_SERVICE, TEST_ACCOUNT);

    /* --- exists() on a non-existent entry --- */
    ASSERT(keychain_exists(TEST_SERVICE, TEST_ACCOUNT) == 0,
           "exists() is 0 before set()");

    /* --- get() on a non-existent entry returns NULL --- */
    char *got = keychain_get(TEST_SERVICE, TEST_ACCOUNT);
    ASSERT(got == NULL, "get() returns NULL for missing entry");

    /* --- set() stores a new entry --- */
    int ok = keychain_set(TEST_SERVICE, TEST_ACCOUNT, TEST_SECRET);
    ASSERT(ok == 1, "set() returns 1 on success");

    /* --- exists() after set() --- */
    ASSERT(keychain_exists(TEST_SERVICE, TEST_ACCOUNT) == 1,
           "exists() is 1 after set()");

    /* --- get() retrieves the stored secret --- */
    got = keychain_get(TEST_SERVICE, TEST_ACCOUNT);
    ASSERT(got != NULL, "get() returns non-NULL after set()");
    ASSERT_STREQ(got, TEST_SECRET, "get() returns the correct secret");
    free(got);

    /* --- set() updates an existing entry --- */
    ok = keychain_set(TEST_SERVICE, TEST_ACCOUNT, TEST_SECRET2);
    ASSERT(ok == 1, "set() returns 1 when updating an existing entry");

    got = keychain_get(TEST_SERVICE, TEST_ACCOUNT);
    ASSERT(got != NULL, "get() returns non-NULL after update");
    ASSERT_STREQ(got, TEST_SECRET2, "get() returns the updated secret");
    free(got);

    /* --- delete() removes the entry --- */
    int deleted = keychain_delete(TEST_SERVICE, TEST_ACCOUNT);
    ASSERT(deleted == 1, "delete() returns 1 on success");

    ASSERT(keychain_exists(TEST_SERVICE, TEST_ACCOUNT) == 0,
           "exists() is 0 after delete()");

    got = keychain_get(TEST_SERVICE, TEST_ACCOUNT);
    ASSERT(got == NULL, "get() returns NULL after delete()");

    /* --- delete() on a non-existent entry returns 0 --- */
    deleted = keychain_delete(TEST_SERVICE, TEST_ACCOUNT);
    ASSERT(deleted == 0, "delete() returns 0 for missing entry");
}

int main(void)
{
    printf("=== std.keychain tests ===\n");

    test_is_available();
    test_null_args();

    if (keychain_is_available()) {
        printf("--- keychain available: running functional tests ---\n");
        test_functional();
    } else {
        printf("--- keychain unavailable on this platform: skipping functional tests ---\n");
    }

    if (failures == 0) {
        printf("All tests passed.\n");
        return 0;
    } else {
        fprintf(stderr, "%d test(s) FAILED.\n", failures);
        return 1;
    }
}
