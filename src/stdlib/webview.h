#ifndef TK_STDLIB_WEBVIEW_H
#define TK_STDLIB_WEBVIEW_H

/*
 * webview.h — C interface for the std.webview standard library module.
 *
 * Embeds a native browser window backed by the platform's system web engine.
 * On macOS the implementation uses WKWebView via the Cocoa and WebKit
 * frameworks.  On other platforms all functions are safe stubs.
 *
 * COMPILATION NOTE:
 *   On macOS (when __APPLE__ is defined) the companion implementation file
 *   (webview.c) must be compiled as Objective-C.  Pass `-x objective-c` to
 *   the C compiler, or rename/symlink webview.c to webview.m:
 *
 *     cc -x objective-c ... -framework WebKit -framework Cocoa webview.c
 *
 *   On all other platforms webview.c compiles as plain C99 with no external
 *   dependencies.
 *
 * Type mappings:
 *   str   = const char *  (null-terminated UTF-8)
 *   i32   = int32_t
 *   bool  = int  (0 = false, non-zero = true)
 *
 * Story: 72.4.1–72.4.6
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------
 * Opaque handle type
 * ------------------------------------------------------------------ */

/*
 * TkWebviewHandle — wraps the platform window object.
 * The id field is a heap-allocated, null-terminated string.
 * An invalid handle has id == NULL or id[0] == '\0'.
 */
typedef struct {
    char *id; /* heap-allocated; freed by tk_webview_close() */
} TkWebviewHandle;

/* ------------------------------------------------------------------
 * Callback function pointer types
 * ------------------------------------------------------------------ */

/* Callback invoked when the window's close button is pressed. */
typedef void (*TkWebviewCloseCb)(void *userdata);

/*
 * Handler callback: receives a null-terminated JSON data string and must
 * return a heap-allocated null-terminated result string.  The caller (this
 * module) takes ownership of the returned pointer and frees it with free().
 */
typedef char *(*TkWebviewHandlerCb)(const char *data, void *userdata);

/* ------------------------------------------------------------------
 * API
 * ------------------------------------------------------------------ */

/*
 * tk_webview_open(url, title, width, height) -> TkWebviewHandle
 *
 * Opens a new native browser window loading `url`, with `title` as the
 * window title and the given pixel dimensions.  On unsupported platforms
 * returns a handle with id == NULL.
 *
 * The caller must eventually call tk_webview_close() to release resources.
 */
TkWebviewHandle tk_webview_open(const char *url, const char *title,
                                 int32_t width, int32_t height);

/*
 * tk_webview_close(h) -> int
 *
 * Closes the window and releases all associated resources, including the
 * memory for h->id.  Returns 1 on success, 0 if the handle is invalid.
 * After this call the handle must not be used again.
 */
int tk_webview_close(TkWebviewHandle *h);

/*
 * tk_webview_set_title(h, title) -> int
 *
 * Updates the title bar text.  Returns 1 on success, 0 on failure.
 */
int tk_webview_set_title(TkWebviewHandle *h, const char *title);

/*
 * tk_webview_on_close(h, cb, userdata)
 *
 * Registers a callback to be invoked when the user closes the window.
 * Replaces any previously registered callback for this handle.
 * `userdata` is passed through to cb unchanged.
 */
void tk_webview_on_close(TkWebviewHandle *h,
                          TkWebviewCloseCb cb, void *userdata);

/*
 * tk_webview_register_handler(h, name, cb, userdata) -> int
 *
 * Registers a named message handler callable from JavaScript as:
 *
 *   window.toke.<name>(dataString)
 *
 * When JS calls this function, `cb` is invoked with the data string and
 * `userdata`.  The string returned by `cb` is posted back to JS as the
 * resolved value.  Only handlers explicitly registered here are reachable
 * from JS (sandbox enforcement).
 *
 * On macOS uses WKScriptMessageHandler.
 * Returns 1 on success, 0 on failure.
 */
int tk_webview_register_handler(TkWebviewHandle *h, const char *name,
                                 TkWebviewHandlerCb cb, void *userdata);

/*
 * tk_webview_eval_js(h, js) -> int
 *
 * Dispatches a JavaScript string for evaluation in the web view context.
 * Returns 1 if dispatched, 0 on failure.  Errors inside the JS itself are
 * not reported back to the caller.
 */
int tk_webview_eval_js(TkWebviewHandle *h, const char *js);

/*
 * tk_webview_run_event_loop()
 *
 * Runs the platform event loop, blocking the calling thread until all
 * webview windows have been closed.  On macOS this starts the NSApplication
 * run loop.  Must be called from the main thread.
 */
void tk_webview_run_event_loop(void);

/*
 * tk_webview_is_available() -> int
 *
 * Returns 1 if the webview module is supported on the current platform,
 * 0 otherwise.  Safe to call at any time with no side effects.
 */
int tk_webview_is_available(void);

#ifdef __cplusplus
}
#endif

#endif /* TK_STDLIB_WEBVIEW_H */
