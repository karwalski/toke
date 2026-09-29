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
 *   webview.c is plain C99 on every platform: on macOS it reaches WebKit
 *   through the Objective-C runtime C API, so no `-x objective-c` is needed
 *   (verified 2026-09-26, 122.11: `cc -std=c99 -Wpedantic -Werror -c`).
 *   It LINKS on macOS only with -framework WebKit -framework Cocoa -lobjc.
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

/* ------------------------------------------------------------------
 * Bridge trust policy (story 122.11)
 *
 * The native bridge -- window.toke.<name>() handlers, the reply and
 * tk_webview_eval_js() (both evaluateJavaScript:) -- is only honoured for
 * a TRUSTED origin:
 *
 *   file:  any file:// document (local content shipped with the app)
 *   other  only an origin added with tk_webview_allow_origin()
 *
 * Messages must also come from the MAIN frame: a message from any
 * sub-frame is dropped, whatever its origin.  Everything else (a remote
 * page reached by redirect or link, an ad iframe, about:blank) is
 * denied: its messages are dropped and eval_js returns 0.  (136.45
 * withdrew the consumer-specific `loke:` scheme that was trusted here.)
 *
 * UNSAFE CASE: loading an untrusted or remote URL into a bridged webview
 * and then allowing its origin hands that page every registered handler.
 * Only allow origins you control, over https.
 *
 * The helpers below are platform-independent (compiled on every host) so
 * the policy can be tested without a display.
 * ------------------------------------------------------------------ */

#include <stddef.h>

#define TK_WEBVIEW_MAX_ORIGINS 16   /* allowlist size per window */

typedef struct {
    char scheme[16];    /* lower-case, e.g. "https"                   */
    char host[256];     /* lower-case; "" for file:                   */
    int  port;          /* 0 = the scheme's default port              */
} TkWebviewOrigin;

/*
 * tk_webview_allow_origin(h, origin) -> int
 * Adds "scheme://host[:port]" to the window's trusted origins.  Any path
 * is ignored.  Returns 1 on success, 0 on a malformed origin, a full list
 * or an invalid handle.
 */
int tk_webview_allow_origin(TkWebviewHandle *h, const char *origin);

/* Parse the origin of a URL.  Returns 1 on success, 0 if malformed.
 * Userinfo is stripped ("https://a.com@b.com" is origin b.com). */
int tk_webview_origin_parse(const char *url, TkWebviewOrigin *out);

/* Returns 1 if o is trusted under the policy above, else 0. */
int tk_webview_origin_trusted(const TkWebviewOrigin *o,
                              const TkWebviewOrigin *allow, int nallow);

/* Handler names are spliced into JavaScript, so only [A-Za-z][A-Za-z0-9]{0,63}
 * is accepted.  Returns 1 if valid. */
int tk_webview_handler_name_ok(const char *name);

/* Escape s for the inside of a double-quoted JavaScript string literal:
 * quote, backslash, every control character, '<', '>', U+2028 and U+2029
 * become escapes.  Returns a malloc'd string (never truncated), or NULL
 * on allocation failure. */
char *tk_webview_js_escape(const char *s);

#ifdef __cplusplus
}
#endif

#endif /* TK_STDLIB_WEBVIEW_H */
