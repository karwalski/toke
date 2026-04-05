/*
 * test_env.c — Unit tests for the std.env C library (Story 2.7.2, 28.5.1).
 *
 * Build and run: make test-stdlib-env
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
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

    /* --- Test 12: env_list returns non-NULL with len > 0 (PATH is set) ----- */
    {
        EnvStrArray list = env_list();
        ASSERT(list.data != NULL, "env_list() data is non-NULL");
        ASSERT(list.len > 0, "env_list() len > 0");

        /* Verify PATH appears somewhere in the list */
        int found_path = 0;
        for (uint64_t i = 0; i < list.len; i++) {
            if (list.data[i] && strcmp(list.data[i], "PATH") == 0) {
                found_path = 1;
            }
            free((void *)list.data[i]);
        }
        free((void *)list.data);
        ASSERT(found_path, "env_list() contains PATH");
    }

    /* --- Test 13: env_delete — set, delete, verify gone ------------------- */
    {
        int set_ok = env_set("TOKE_TEST_DEL", "to_be_deleted");
        ASSERT(set_ok == 1, "env_set(TOKE_TEST_DEL) succeeds before delete");
        int del_ok = env_delete("TOKE_TEST_DEL");
        ASSERT(del_ok == 1, "env_delete(TOKE_TEST_DEL) returns 1");
        EnvGetResult r = env_get("TOKE_TEST_DEL");
        ASSERT(r.is_err, "env_get after env_delete returns error");
        ASSERT(r.err_kind == ENV_ERR_NOT_FOUND,
               "env_get after env_delete err_kind == NotFound");
    }

    /* --- Test 14: env_expand with $USER ----------------------------------- */
    {
        const char *user = getenv("USER");
        if (user && *user) {
            char *expanded = env_expand("Hello $USER world");
            ASSERT(expanded != NULL, "env_expand() returns non-NULL");
            ASSERT(strstr(expanded, user) != NULL,
                   "env_expand($USER) contains actual USER value");
            free(expanded);
        } else {
            printf("pass: env_expand($USER) skipped (USER not set)\n");
        }
    }

    /* --- Test 15: env_expand with $$ → literal '$' ------------------------ */
    {
        char *expanded = env_expand("$$PATH");
        ASSERT(expanded != NULL, "env_expand($$PATH) returns non-NULL");
        ASSERT_STREQ(expanded, "$PATH", "env_expand($$PATH) == \"$PATH\"");
        free(expanded);
    }

    /* --- Test 16: env_expand with ${VAR} syntax --------------------------- */
    {
        env_set("TOKE_TEST_EXP", "expanded_val");
        char *expanded = env_expand("prefix_${TOKE_TEST_EXP}_suffix");
        ASSERT(expanded != NULL, "env_expand(${VAR}) returns non-NULL");
        ASSERT_STREQ(expanded, "prefix_expanded_val_suffix",
                     "env_expand(${TOKE_TEST_EXP}) substitutes correctly");
        free(expanded);
    }

    /* --- Test 17: env_expand unknown var → empty string ------------------- */
    {
        unsetenv("TOKE_TEST_UNDEFINED_XYZ");
        char *expanded = env_expand("before.$TOKE_TEST_UNDEFINED_XYZ.after");
        ASSERT(expanded != NULL, "env_expand(unknown var) returns non-NULL");
        ASSERT_STREQ(expanded, "before..after",
                     "env_expand(unknown var) substitutes empty string");
        free(expanded);
    }

    /* --- Test 18: env_file_load with KEY=VALUE pairs ---------------------- */
    {
        /* Create a temp file */
        char tmppath[] = "/tmp/toke_test_envfile_XXXXXX";
        int  fd = mkstemp(tmppath);
        ASSERT(fd >= 0, "env_file_load: mkstemp succeeds");
        if (fd >= 0) {
            const char *content =
                "# comment line\n"
                "\n"
                "TOKE_FL_PLAIN=hello\n"
                "TOKE_FL_DQ=\"quoted value\"\n"
                "TOKE_FL_SQ='single quoted'\n"
                "TOKE_FL_ESC=\"line1\\nline2\"\n"
                "  TOKE_FL_TRIM  =  trimmed  \n";
            write(fd, content, strlen(content));
            close(fd);

            int n = env_file_load(tmppath);
            ASSERT(n == 5, "env_file_load returns 5 (vars set)");

            EnvGetResult r1 = env_get("TOKE_FL_PLAIN");
            ASSERT(!r1.is_err && r1.ok && strcmp(r1.ok, "hello") == 0,
                   "env_file_load: plain KEY=VALUE");

            EnvGetResult r2 = env_get("TOKE_FL_DQ");
            ASSERT(!r2.is_err && r2.ok && strcmp(r2.ok, "quoted value") == 0,
                   "env_file_load: double-quoted value");

            EnvGetResult r3 = env_get("TOKE_FL_SQ");
            ASSERT(!r3.is_err && r3.ok && strcmp(r3.ok, "single quoted") == 0,
                   "env_file_load: single-quoted value");

            EnvGetResult r4 = env_get("TOKE_FL_ESC");
            ASSERT(!r4.is_err && r4.ok && strcmp(r4.ok, "line1\nline2") == 0,
                   "env_file_load: escape \\n in double-quoted value");

            EnvGetResult r5 = env_get("TOKE_FL_TRIM");
            ASSERT(!r5.is_err && r5.ok && strcmp(r5.ok, "trimmed") == 0,
                   "env_file_load: whitespace trimmed from key and value");

            unlink(tmppath);
        }
    }

    /* --- Test 19: env_file_load on non-existent file returns -1 ----------- */
    {
        int n = env_file_load("/tmp/toke_no_such_file_xyz_abc_123.env");
        ASSERT(n == -1, "env_file_load(missing file) returns -1");
    }

    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
