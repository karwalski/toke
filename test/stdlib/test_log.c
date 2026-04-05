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

    /* ---- Story 28.5.3 tests -------------------------------------------- */

    /* Reset level to DEBUG so debug messages pass through. */
    tk_log_set_level("DEBUG");
    /* Reset format to JSON (default) for a clean slate. */
    tk_log_set_format("json");

    /* ---- Test 5: tk_log_debug — emitted when level == DEBUG ------------ */
    redirect_stderr_to(tmp);
    const char *fields5[] = {"component", "parser"};
    int r5 = tk_log_debug("entering parse loop", fields5, 2);
    fflush(stderr);
    restore_stderr();

    ASSERT(r5 == 1, "log.debug returns 1 when level == DEBUG");

    char *out5 = read_file(tmp);
    ASSERT(out5 != NULL, "captured log.debug output");
    if (out5) {
        ASSERT(strstr(out5, "debug") != NULL, "log.debug output contains \"debug\"");
        free(out5);
    }

    /* ---- Test 6: tk_log_debug — filtered when level == INFO ------------ */
    tk_log_set_level("INFO");

    redirect_stderr_to(tmp);
    int r6 = tk_log_debug("should be filtered", NULL, 0);
    fflush(stderr);
    restore_stderr();

    ASSERT(r6 == 0, "log.debug returns 0 when level == INFO (filtered)");

    /* ---- Test 7: tk_log_set_format("json") — output is JSON line ------- */
    tk_log_set_level("INFO");
    tk_log_set_format("json");

    const char *out7_path = "/tmp/test_log_json.txt";
    ASSERT(tk_log_set_output(out7_path) == 1, "tk_log_set_output to JSON test file succeeds");

    const char *fields7[] = {"key", "val"};
    tk_log_info("json format test", fields7, 2);

    /* Reset output back to stderr before reading the file. */
    tk_log_set_output(NULL);

    char *out7 = read_file(out7_path);
    ASSERT(out7 != NULL, "captured JSON format output");
    if (out7) {
        ASSERT(strstr(out7, "{")        != NULL, "json format: line starts with {");
        ASSERT(strstr(out7, "\"level\"") != NULL, "json format: contains \"level\" key");
        ASSERT(strstr(out7, "\"info\"")  != NULL, "json format: contains \"info\" value");
        ASSERT(strstr(out7, "\"msg\"")   != NULL, "json format: contains \"msg\" key");
        ASSERT(strstr(out7, "\"ts\"")    != NULL, "json format: contains \"ts\" key");
        free(out7);
    }

    /* ---- Test 8: tk_log_set_format("text") — output contains [INFO] ---- */
    tk_log_set_format("text");

    const char *out8_path = "/tmp/test_log_text.txt";
    ASSERT(tk_log_set_output(out8_path) == 1, "tk_log_set_output to text test file succeeds");

    const char *fields8[] = {"x", "1"};
    tk_log_info("text format test", fields8, 2);

    tk_log_set_output(NULL);
    tk_log_set_format("json"); /* restore default */

    char *out8 = read_file(out8_path);
    ASSERT(out8 != NULL, "captured text format output");
    if (out8) {
        ASSERT(strstr(out8, "[INFO]") != NULL, "text format: contains [INFO]");
        ASSERT(strstr(out8, "text format test") != NULL, "text format: contains message");
        ASSERT(strstr(out8, "x=1") != NULL, "text format: contains key=val pair");
        free(out8);
    }

    /* ---- Test 9: tk_log_set_output — redirects to file ----------------- */
    const char *out9_path = "/tmp/test_log_redirect.txt";
    ASSERT(tk_log_set_output(out9_path) == 1, "tk_log_set_output returns 1 on success");

    tk_log_info("redirected message", NULL, 0);
    tk_log_set_output(NULL); /* back to stderr */

    char *out9 = read_file(out9_path);
    ASSERT(out9 != NULL, "redirected output file is readable");
    if (out9) {
        ASSERT(strstr(out9, "redirected message") != NULL,
               "redirected file contains the logged message");
        free(out9);
    }

    /* ---- Test 10: tk_log_set_output(NULL) — reset to stderr ------------ */
    /* Just verify it doesn't crash and subsequent log calls still work. */
    tk_log_set_output(NULL);
    redirect_stderr_to(tmp);
    int r10 = tk_log_info("back to stderr", NULL, 0);
    fflush(stderr);
    restore_stderr();
    ASSERT(r10 == 1, "log.info returns 1 after resetting output to stderr");

    /* ---- Test 11: tk_log_with_context — fields from JSON --------------- */
    tk_log_set_format("json");
    const char *out11_path = "/tmp/test_log_ctx.txt";
    ASSERT(tk_log_set_output(out11_path) == 1, "tk_log_set_output for context test");

    int r11 = tk_log_with_context("context log",
                                  "{\"request_id\":\"xyz\",\"user\":\"carol\"}");
    tk_log_set_output(NULL);

    ASSERT(r11 == 1, "tk_log_with_context returns 1 when level >= INFO");

    char *out11 = read_file(out11_path);
    ASSERT(out11 != NULL, "captured tk_log_with_context output");
    if (out11) {
        ASSERT(strstr(out11, "context log")  != NULL, "with_context: message present");
        ASSERT(strstr(out11, "request_id")   != NULL, "with_context: request_id key present");
        ASSERT(strstr(out11, "xyz")          != NULL, "with_context: request_id value present");
        ASSERT(strstr(out11, "user")         != NULL, "with_context: user key present");
        ASSERT(strstr(out11, "carol")        != NULL, "with_context: user value present");
        free(out11);
    }

    /* ---- Test 12: tk_log_with_context — NULL context is safe ----------- */
    redirect_stderr_to(tmp);
    int r12 = tk_log_with_context("no context", NULL);
    fflush(stderr);
    restore_stderr();
    ASSERT(r12 == 1, "tk_log_with_context with NULL context doesn't crash");

    /* ---- Summary ------------------------------------------------------- */
    if (failures == 0) { printf("All log tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
