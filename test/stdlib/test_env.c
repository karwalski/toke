/*
 * test_env.c — Unit tests for the std.env C library (Story 2.7.2).
 *
 * Build and run: make test-stdlib-env
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/stdlib/env.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

#define ASSERT_STREQ(a, b, msg) \
    ASSERT((a) && (b) && strcmp((a),(b)) == 0, msg)

int main(void)
{
    /* --- Test 1: env_get on a known variable (PATH) ----------------------- */
    {
        EnvGetResult r = env_get("PATH");
        ASSERT(!r.is_err, "env_get(PATH) succeeds");
        ASSERT(r.ok != NULL, "env_get(PATH) returns non-NULL");
    }

    /* --- Test 2: env_get on a missing variable returns EnvErr ------------- */
    {
        /* Use a name unlikely to be set */
        unsetenv("TOKE_TEST_MISSING_XYZ");
        EnvGetResult r = env_get("TOKE_TEST_MISSING_XYZ");
        ASSERT(r.is_err, "env_get(missing) is error");
        ASSERT(r.err_kind == ENV_ERR_NOT_FOUND,
               "env_get(missing) err_kind == NotFound");
    }

    /* --- Test 3: env_set then env_get round-trip -------------------------- */
    {
        int set_ok = env_set("TOKE_TEST_VAR", "hello_env");
        ASSERT(set_ok == 1, "env_set(TOKE_TEST_VAR) returns 1");
        EnvGetResult r = env_get("TOKE_TEST_VAR");
        ASSERT(!r.is_err, "env_get after set succeeds");
        ASSERT_STREQ(r.ok, "hello_env", "env_get returns set value");
    }

    /* --- Test 4: env_set overwrites existing value ------------------------ */
    {
        env_set("TOKE_TEST_OVR", "first");
        env_set("TOKE_TEST_OVR", "second");
        EnvGetResult r = env_get("TOKE_TEST_OVR");
        ASSERT(!r.is_err, "env_get after overwrite succeeds");
        ASSERT_STREQ(r.ok, "second", "env_get returns overwritten value");
    }

    /* --- Test 5: env_get_or returns value when key is present ------------- */
    {
        env_set("TOKE_TEST_GETOR", "present");
        const char *val = env_get_or("TOKE_TEST_GETOR", "fallback");
        ASSERT_STREQ(val, "present", "env_get_or returns present value");
    }

    /* --- Test 6: env_get_or returns default when key is absent ------------ */
    {
        unsetenv("TOKE_TEST_ABSENT_ABC");
        const char *val = env_get_or("TOKE_TEST_ABSENT_ABC", "my_default");
        ASSERT_STREQ(val, "my_default", "env_get_or returns default when absent");
    }

    /* --- Test 7: EnvErr Invalid for key containing '=' ------------------- */
    {
        EnvGetResult r = env_get("BAD=KEY");
        ASSERT(r.is_err, "env_get(bad=key) is error");
        ASSERT(r.err_kind == ENV_ERR_INVALID,
               "env_get(bad=key) err_kind == Invalid");
    }

    /* --- Test 8: env_set with invalid key returns 0 ----------------------- */
    {
        int ok = env_set("BAD=KEY", "val");
        ASSERT(ok == 0, "env_set(bad=key) returns 0");
    }

    /* --- Test 9: env_set with empty key returns 0 ------------------------- */
    {
        int ok = env_set("", "val");
        ASSERT(ok == 0, "env_set(empty key) returns 0");
    }

    /* --- Test 10: env_get with empty key returns Invalid error ------------ */
    {
        EnvGetResult r = env_get("");
        ASSERT(r.is_err, "env_get(empty key) is error");
        ASSERT(r.err_kind == ENV_ERR_INVALID,
               "env_get(empty key) err_kind == Invalid");
    }

    /* --- Test 11: env_get_or with invalid key returns default ------------- */
    {
        const char *val = env_get_or("BAD=KEY", "safe_default");
        ASSERT_STREQ(val, "safe_default",
                     "env_get_or(bad key) returns default");
    }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
