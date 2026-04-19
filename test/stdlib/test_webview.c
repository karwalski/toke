/*
 * test_webview.c — Unit tests for the std.webview C library (Story 72.4.4).
 *
 * These tests exercise the platform-independent surface of the API:
 *   - is_available() returns the correct value for the platform
 *   - open() on an unsupported platform returns an invalid handle
 *   - close(), set_title(), register_handler(), eval_js() are safe with
 *     invalid handles (no crash, correct return values)
 *   - on_close() is a safe no-op with an invalid handle
 *
 * On macOS the tests are intentionally headless — we do not call
 * tk_webview_open() with a real URL because that would require a display
 * and a running NSApplication event loop, which is unsuitable for CI.
 * The macOS-specific path is validated by the build verification step
 * (compiling with -x objective-c).
 *
 * Build and run:
 *   cc -Wall -Wextra -Isrc -Isrc/stdlib \
 *      test/stdlib/test_webview.c -o /tmp/test_webview && /tmp/test_webview
 *
 * Build and run (macOS, Objective-C):
 *   cc -Wall -Wextra -Isrc -Isrc/stdlib -x objective-c \
 *      -framework WebKit -framework Cocoa \
 *      src/stdlib/webview.c test/stdlib/test_webview.c \
 *      -o /tmp/test_webview && /tmp/test_webview
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/stdlib/webview.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { \
        if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
        else         { printf("pass: %s\n", msg); } \
    } while (0)

/* -----------------------------------------------------------------------
 * Shared test helpers
 * ----------------------------------------------------------------------- */

static int close_cb_called = 0;
static void dummy_close_cb(void *userdata)
{
    (void)userdata;
    close_cb_called = 1;
}

static char *echo_handler(const char *data, void *userdata)
{
    (void)userdata;
    if (!data) return strdup("");
    return strdup(data);
}

/* -----------------------------------------------------------------------
 * Tests for is_available()
 * ----------------------------------------------------------------------- */

static void test_is_available(void)
{
#if defined(__APPLE__)
    ASSERT(tk_webview_is_available() == 1,
           "is_available() == 1 on macOS");
#else
    ASSERT(tk_webview_is_available() == 0,
           "is_available() == 0 on non-Apple platform");
#endif
}

/* -----------------------------------------------------------------------
 * Tests with an invalid (NULL id) handle — must not crash
 * ----------------------------------------------------------------------- */

static void test_null_handle_safety(void)
{
    TkWebviewHandle bad;
    bad.id = NULL;

    int r;

    r = tk_webview_close(&bad);
    ASSERT(r == 0, "close(NULL-id handle) returns 0");

    r = tk_webview_set_title(&bad, "title");
    ASSERT(r == 0, "set_title(NULL-id handle) returns 0");

    tk_webview_on_close(&bad, dummy_close_cb, NULL);
    ASSERT(close_cb_called == 0,
           "on_close(NULL-id handle) does not invoke callback");

    r = tk_webview_register_handler(&bad, "test", echo_handler, NULL);
    ASSERT(r == 0, "register_handler(NULL-id handle) returns 0");

    r = tk_webview_eval_js(&bad, "1+1");
    ASSERT(r == 0, "eval_js(NULL-id handle) returns 0");
}

/* -----------------------------------------------------------------------
 * Tests with a NULL TkWebviewHandle pointer — must not crash
 * ----------------------------------------------------------------------- */

static void test_null_pointer_safety(void)
{
    int r;

    r = tk_webview_close(NULL);
    ASSERT(r == 0, "close(NULL) returns 0");

    r = tk_webview_set_title(NULL, "title");
    ASSERT(r == 0, "set_title(NULL) returns 0");

    tk_webview_on_close(NULL, dummy_close_cb, NULL);
    ASSERT(close_cb_called == 0,
           "on_close(NULL) does not invoke callback");

    r = tk_webview_register_handler(NULL, "test", echo_handler, NULL);
    ASSERT(r == 0, "register_handler(NULL) returns 0");

    r = tk_webview_eval_js(NULL, "1+1");
    ASSERT(r == 0, "eval_js(NULL) returns 0");
}

/* -----------------------------------------------------------------------
 * On non-Apple platforms: open() must return an invalid handle
 * ----------------------------------------------------------------------- */

#ifndef __APPLE__
static void test_open_stub(void)
{
    TkWebviewHandle h = tk_webview_open("http://example.com", "Test", 800, 600);
    ASSERT(h.id == NULL,
           "open() on non-Apple platform returns handle with NULL id");

    /* Closing an invalid stub handle must be safe */
    int r = tk_webview_close(&h);
    ASSERT(r == 0, "close() on stub handle returns 0");
}
#endif

/* -----------------------------------------------------------------------
 * open() with NULL arguments — must not crash
 * ----------------------------------------------------------------------- */

static void test_open_null_args(void)
{
    /* These should not crash regardless of platform */
    TkWebviewHandle h1 = tk_webview_open(NULL, "title", 800, 600);
    (void)h1;

    TkWebviewHandle h2 = tk_webview_open("http://example.com", NULL, 800, 600);
    (void)h2;

    /* If we got here without crashing, the test passes */
    ASSERT(1, "open() with NULL args does not crash");

    /* Clean up if handles were somehow allocated */
    if (h1.id) { tk_webview_close(&h1); }
    if (h2.id) { tk_webview_close(&h2); }
}

/* -----------------------------------------------------------------------
 * register_handler: NULL name or NULL cb must return 0 without crash
 * ----------------------------------------------------------------------- */

static void test_register_handler_bad_args(void)
{
    TkWebviewHandle bad;
    bad.id = NULL;

    int r;

    r = tk_webview_register_handler(&bad, NULL, echo_handler, NULL);
    ASSERT(r == 0, "register_handler(NULL name) returns 0");

    r = tk_webview_register_handler(&bad, "test", NULL, NULL);
    ASSERT(r == 0, "register_handler(NULL cb) returns 0");
}

/* -----------------------------------------------------------------------
 * eval_js with NULL js string — must not crash
 * ----------------------------------------------------------------------- */

static void test_eval_js_null(void)
{
    TkWebviewHandle bad;
    bad.id = NULL;

    int r = tk_webview_eval_js(&bad, NULL);
    ASSERT(r == 0, "eval_js(NULL js) returns 0");
}

/* -----------------------------------------------------------------------
 * set_title with NULL title — must not crash
 * ----------------------------------------------------------------------- */

static void test_set_title_null(void)
{
    TkWebviewHandle bad;
    bad.id = NULL;

    int r = tk_webview_set_title(&bad, NULL);
    ASSERT(r == 0, "set_title(NULL title) returns 0");
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */

int main(void)
{
    printf("=== std.webview tests ===\n");

    test_is_available();
    test_null_handle_safety();
    test_null_pointer_safety();

#ifndef __APPLE__
    test_open_stub();
#endif

    test_open_null_args();
    test_register_handler_bad_args();
    test_eval_js_null();
    test_set_title_null();

    printf("=========================\n");
    if (failures == 0) {
        printf("All webview tests passed.\n");
        return 0;
    }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
