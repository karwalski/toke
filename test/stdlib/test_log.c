/*
 * test_log.c — Unit tests for the std.log C library (Story 3.8.1).
 *
 * Build and run: make test-stdlib-log
 *
 * Tests:
 *  1. log.info with 2 key/value fields — output contains "info" and both keys.
 *  2. log.warn — output contains "warn".
 *  3. With TK_LOG_LEVEL=ERROR, log.info returns 0 (filtered).
 *  4. With TK_LOG_LEVEL=ERROR, log.error returns 1 (emitted).
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "../../src/stdlib/log.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

/* -------------------------------------------------------------------------
 * Capture stderr into a buffer so we can inspect log output.
 * We use a temporary file as a pipe-free portable approach.
 * ---------------------------------------------------------------------- */

/*
 * redirect_stderr_to / restore_stderr:
 *   redirect_stderr_to(path) — reopen stderr to write into path.
 *   restore_stderr()         — reopen stderr to /dev/stderr.
 */
static void redirect_stderr_to(const char *path)
{
    freopen(path, "w", stderr);
}

static void restore_stderr(void)
{
    freopen("/dev/stderr", "w", stderr);
}

/*
 * read_file — read the contents of path into a heap-allocated buffer.
 * Caller must free the returned pointer.  Returns NULL on error.
 */
static char *read_file(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* -------------------------------------------------------------------------
 * Tests
 * ---------------------------------------------------------------------- */

int main(void)
{
    const char *tmp = "/tmp/test_log_output.txt";

    /* ---- Test 1: log.info with fields ---------------------------------- */
    setenv("TK_LOG_LEVEL", "INFO", 1);
    /* Reset the cached level so the new env var is picked up. */
    tk_log_set_level("INFO");

    redirect_stderr_to(tmp);
    const char *fields1[] = {"request_id", "abc123", "user", "alice"};
    int r1 = tk_log_info("request received", fields1, 4);
    fflush(stderr);
    restore_stderr();

    ASSERT(r1 == 1, "log.info returns 1 when level >= INFO");

    char *out1 = read_file(tmp);
    ASSERT(out1 != NULL, "captured log.info output");
    if (out1) {
        ASSERT(strstr(out1, "\"info\"") != NULL,   "log.info output contains \"info\"");
        ASSERT(strstr(out1, "request_id") != NULL, "log.info output contains field key request_id");
        ASSERT(strstr(out1, "user") != NULL,       "log.info output contains field key user");
        free(out1);
    }

    /* ---- Test 2: log.warn ---------------------------------------------- */
    redirect_stderr_to(tmp);
    const char *fields2[] = {"code", "429"};
    int r2 = tk_log_warn("rate limit exceeded", fields2, 2);
    fflush(stderr);
    restore_stderr();

    ASSERT(r2 == 1, "log.warn returns 1 when level >= INFO");

    char *out2 = read_file(tmp);
    ASSERT(out2 != NULL, "captured log.warn output");
    if (out2) {
        ASSERT(strstr(out2, "\"warn\"") != NULL, "log.warn output contains \"warn\"");
        free(out2);
    }

    /* ---- Test 3: log.info filtered when level == ERROR ----------------- */
    tk_log_set_level("ERROR");

    redirect_stderr_to(tmp);
    int r3 = tk_log_info("should be filtered", NULL, 0);
    fflush(stderr);
    restore_stderr();

    ASSERT(r3 == 0, "log.info returns 0 when level == ERROR (filtered)");

    /* ---- Test 4: log.error emitted when level == ERROR ----------------- */
    redirect_stderr_to(tmp);
    const char *fields4[] = {"errno", "ENOENT"};
    int r4 = tk_log_error("file not found", fields4, 2);
    fflush(stderr);
    restore_stderr();

    ASSERT(r4 == 1, "log.error returns 1 when level == ERROR");

    char *out4 = read_file(tmp);
    ASSERT(out4 != NULL, "captured log.error output");
    if (out4) {
        ASSERT(strstr(out4, "\"error\"") != NULL, "log.error output contains \"error\"");
        free(out4);
    }

    /* ---- Summary ------------------------------------------------------- */
    if (failures == 0) { printf("All log tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
