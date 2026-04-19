/*
 * webview.c — Implementation of the std.webview standard library module.
 *
 * Platform support:
 *   macOS  — WKWebView via Cocoa + WebKit, accessed through the Objective-C
 *             runtime C API (objc_msgSend / objc_getClass) so this file
 *             compiles as plain C99 without -x objective-c.
 *   Windows — Stub section reserved for future WebView2 integration.
 *   Other  — All functions are safe no-ops; is_available() returns 0.
 *
 * On macOS link with: -framework WebKit -framework Cocoa
 * No -x objective-c flag required; plain cc -c suffices.
 *
 * Sandbox contract:
 *   JavaScript inside the web view may ONLY call handlers registered via
 *   tk_webview_register_handler().  Each handler is exposed as
 *   window.toke.<name>(data).  No other toke functions are reachable.
 *
 * Story: 72.4.1–72.4.6
 */

#include "webview.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>

/* ======================================================================
 * macOS implementation — plain C99 using ObjC runtime C API
 * ====================================================================== */
#ifdef __APPLE__

#include <objc/runtime.h>
#include <objc/message.h>
#include <CoreGraphics/CGGeometry.h>
#include <stddef.h>

/*
 * On LP64 (the only macOS ABI we target) NSInteger == ptrdiff_t and
 * NSUInteger == size_t.  We define local aliases to avoid pulling in
 * NSObjCRuntime.h (which would require ObjC mode or careful ordering).
 */
typedef ptrdiff_t TkNSInteger;
typedef size_t    TkNSUInteger;

/*
 * Function-pointer casts for objc_msgSend.
 * We spell out each call site fully to avoid variadic macro GNU extensions
 * that -Wpedantic rejects.
 */
typedef id   (*ObjcMsg0)(id, SEL);
typedef id   (*ObjcMsg1)(id, SEL, id);

typedef void (*ObjcVoid0)(id, SEL);
typedef void (*ObjcVoid1)(id, SEL, id);
typedef void (*ObjcVoid2)(id, SEL, id, id);
typedef void (*ObjcVoid3)(id, SEL, id, id, id);
typedef BOOL (*ObjcBool1)(id, SEL, id);

/* Typed send helpers — no variadic macros, fully C99 compatible */
static id   ms0(id o, const char *s)
    { return ((ObjcMsg0)objc_msgSend)(o, sel_getUid(s)); }
static id   ms1(id o, const char *s, id a)
    { return ((ObjcMsg1)objc_msgSend)(o, sel_getUid(s), a); }

static void mv0(id o, const char *s)
    { ((ObjcVoid0)objc_msgSend)(o, sel_getUid(s)); }
static void mv1(id o, const char *s, id a)
    { ((ObjcVoid1)objc_msgSend)(o, sel_getUid(s), a); }
static void mv2(id o, const char *s, id a, id b)
    { ((ObjcVoid2)objc_msgSend)(o, sel_getUid(s), a, b); }

/* Helpers for integer/bool/CGRect arguments that don't fit id */
typedef id   (*ObjcMsgNSInt1)(id, SEL, TkNSInteger);
typedef id   (*ObjcMsgRect)(id, SEL, CGRect);
typedef id   (*ObjcMsgRect2)(id, SEL, CGRect, TkNSUInteger, TkNSUInteger, BOOL);
typedef id   (*ObjcMsgRectCfg)(id, SEL, CGRect, id);
typedef void (*ObjcVoidBool)(id, SEL, BOOL);
typedef void (*ObjcVoidNSInt)(id, SEL, TkNSInteger);
typedef id   (*ObjcMsgBytes)(id, SEL, const void *, TkNSUInteger);
typedef id   (*ObjcMsgUrlMimeLen)(id, SEL, id, id, TkNSInteger, id);
typedef id   (*ObjcMsgSrcTimeMain)(id, SEL, id, TkNSInteger, BOOL);

static id ms_rect_cfg(id o, const char *s, CGRect r, id cfg)
    { return ((ObjcMsgRectCfg)objc_msgSend)(o, sel_getUid(s), r, cfg); }
static id ms_rect_style(id o, const char *s, CGRect r, TkNSUInteger style,
                         TkNSUInteger backing, BOOL defer)
    { return ((ObjcMsgRect2)objc_msgSend)(o, sel_getUid(s), r, style, backing, defer); }
static void mv_bool(id o, const char *s, BOOL v)
    { ((ObjcVoidBool)objc_msgSend)(o, sel_getUid(s), v); }
static void mv_nsint(id o, const char *s, TkNSInteger v)
    { ((ObjcVoidNSInt)objc_msgSend)(o, sel_getUid(s), v); }
static id ms_bytes(id o, const char *s, const void *b, TkNSUInteger len)
    { return ((ObjcMsgBytes)objc_msgSend)(o, sel_getUid(s), b, len); }
static id ms_url_mime_len_enc(id o, const char *s, id url, id mime,
                               TkNSInteger len, id enc)
    { return ((ObjcMsgUrlMimeLen)objc_msgSend)(o, sel_getUid(s), url, mime, len, enc); }
static id ms_src_time_main(id o, const char *s, id src, TkNSInteger t, BOOL main_only)
    { return ((ObjcMsgSrcTimeMain)objc_msgSend)(o, sel_getUid(s), src, t, main_only); }

/* ------------------------------------------------------------------
 * Internal handler registry
 * ------------------------------------------------------------------ */

#define TK_WEBVIEW_MAX_HANDLERS 64

typedef struct {
    char                name[128];
    TkWebviewHandlerCb  cb;
    void               *userdata;
} TkHandlerEntry;

/* ------------------------------------------------------------------
 * Platform window state
 * ------------------------------------------------------------------ */

typedef struct TkWebviewState {
    id  window;    /* NSWindow  */
    id  webview;   /* WKWebView */
    id  config;    /* WKWebViewConfiguration */
    id  ucc;       /* WKUserContentController */
    id  delegate;  /* TkWebviewDelegate (NSObject subclass) */

    TkWebviewCloseCb    close_cb;
    void               *close_userdata;

    TkHandlerEntry      handlers[TK_WEBVIEW_MAX_HANDLERS];
    int                 handler_count;
} TkWebviewState;

/* ------------------------------------------------------------------
 * Helper: create NSString from C string
 * ------------------------------------------------------------------ */
static id ns_string(const char *s)
{
    if (!s) s = "";
    return ms1((id)objc_getClass("NSString"),
               "stringWithUTF8String:",
               (id)(uintptr_t)s);   /* cast: string pointer fits through id on LP64 */
}

/*
 * NOTE: passing a const char * through an id parameter works on LP64 macOS
 * because pointers are the same width, and objc_msgSend treats the argument
 * as a register-sized value.  The NSString class method expects a const char *
 * in that slot, which is exactly what we provide.
 */

/* ------------------------------------------------------------------
 * Inject bootstrap JavaScript that wires up window.toke.<name>()
 * ------------------------------------------------------------------ */
static void tk_inject_bootstrap(TkWebviewState *st)
{
    char buf[16384];
    int pos = 0;

    pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
        "(function(){\n"
        "  if(!window.toke) window.toke={};\n");

    for (int i = 0; i < st->handler_count; i++) {
        if (strcmp(st->handlers[i].name, "loke_scheme") == 0) continue;
        pos += snprintf(buf + pos, sizeof(buf) - (size_t)pos,
            "  window.toke['%s']=function(data){\n"
            "    return new Promise(function(resolve){\n"
            "      var key='_toke_reply_%s';\n"
            "      window.addEventListener(key,function h(e){\n"
            "        window.removeEventListener(key,h);\n"
            "        resolve(e.detail);\n"
            "      });\n"
            "      window.webkit.messageHandlers['%s'].postMessage(\n"
            "        typeof data==='string'?data:JSON.stringify(data));\n"
            "    });\n"
            "  };\n",
            st->handlers[i].name,
            st->handlers[i].name,
            st->handlers[i].name);
    }
    snprintf(buf + pos, sizeof(buf) - (size_t)pos, "})();\n");

    /* Remove existing user scripts */
    mv0(st->ucc, "removeAllUserScripts");

    /* Build WKUserScript(source:injectionTime:forMainFrameOnly:) */
    id src    = ns_string(buf);
    id script = ms_src_time_main(
        ms0((id)objc_getClass("WKUserScript"), "alloc"),
        "initWithSource:injectionTime:forMainFrameOnly:",
        src,
        (TkNSInteger)0,   /* WKUserScriptInjectionTimeAtDocumentStart */
        NO
    );
    mv1(st->ucc, "addUserScript:", script);
}

/* ------------------------------------------------------------------
 * Delegate implementation via ObjC runtime
 * ------------------------------------------------------------------ */

/* IMP for userContentController:didReceiveScriptMessage: */
static void delegate_did_receive_message(id self, SEL cmd,
                                          id controller, id message)
{
    (void)cmd; (void)controller;
    TkWebviewState *st = NULL;
    object_getInstanceVariable(self, "state", (void **)&st);
    if (!st) return;

    id name_obj = ms0(message, "name");
    const char *handler_name = (const char *)ms0(name_obj, "UTF8String");
    if (!handler_name) return;

    id body = ms0(message, "body");
    const char *body_str = "";
    if (body) {
        BOOL is_str = ((ObjcBool1)objc_msgSend)(body, sel_getUid("isKindOfClass:"),
                                                 (id)objc_getClass("NSString"));
        if (is_str) {
            const char *tmp = (const char *)ms0(body, "UTF8String");
            if (tmp) body_str = tmp;
        }
    }

    for (int i = 0; i < st->handler_count; i++) {
        if (strcmp(st->handlers[i].name, handler_name) == 0) {
            char *result = st->handlers[i].cb(body_str,
                                               st->handlers[i].userdata);
            if (result) {
                /* Escape result for inline JS string literal */
                char escaped[8192];
                size_t out = 0;
                for (size_t k = 0; result[k] && out < sizeof(escaped) - 3; k++) {
                    if      (result[k] == '"')  { escaped[out++] = '\\'; escaped[out++] = '"';  }
                    else if (result[k] == '\\') { escaped[out++] = '\\'; escaped[out++] = '\\'; }
                    else                         { escaped[out++] = result[k]; }
                }
                escaped[out] = '\0';

                char js[8256];
                snprintf(js, sizeof(js),
                    "(function(){"
                    "var e=new CustomEvent('_toke_reply_%s',{detail:\"%s\"});"
                    "window.dispatchEvent(e);"
                    "})()",
                    handler_name, escaped);

                mv2(st->webview, "evaluateJavaScript:completionHandler:",
                    ns_string(js), (id)nil);
                free(result);
            }
            return;
        }
    }
}

/* IMP for windowWillClose: */
static void delegate_window_will_close(id self, SEL cmd, id notification)
{
    (void)cmd; (void)notification;
    TkWebviewState *st = NULL;
    object_getInstanceVariable(self, "state", (void **)&st);
    if (st && st->close_cb) {
        st->close_cb(st->close_userdata);
    }
}

static Class tk_delegate_class(void)
{
    static Class cls = NULL;
    if (cls) return cls;

    cls = objc_allocateClassPair(objc_getClass("NSObject"),
                                  "TkWebviewDelegate", 0);
    if (!cls) return NULL;

    class_addIvar(cls, "state", sizeof(void *),
                  (uint8_t)__builtin_ctz(sizeof(void *)), "^v");

    Protocol *wk_h = objc_getProtocol("WKScriptMessageHandler");
    if (wk_h) class_addProtocol(cls, wk_h);
    Protocol *wd   = objc_getProtocol("NSWindowDelegate");
    if (wd)   class_addProtocol(cls, wd);

    class_addMethod(cls,
        sel_getUid("userContentController:didReceiveScriptMessage:"),
        (IMP)delegate_did_receive_message, "v@:@@");
    class_addMethod(cls,
        sel_getUid("windowWillClose:"),
        (IMP)delegate_window_will_close, "v@:@");

    objc_registerClassPair(cls);
    return cls;
}

/* ------------------------------------------------------------------
 * loke:// URL scheme handler
 * ------------------------------------------------------------------ */

static void scheme_start(id self, SEL cmd, id webview, id task)
{
    (void)cmd; (void)webview;
    TkWebviewState *st = NULL;
    object_getInstanceVariable(self, "state", (void **)&st);

    id request = ms0(task, "request");
    id url_obj = ms0(request, "URL");
    id abs_str = ms0(url_obj, "absoluteString");
    const char *url_str = (const char *)ms0(abs_str, "UTF8String");

    char *result = NULL;
    if (st) {
        for (int i = 0; i < st->handler_count; i++) {
            if (strcmp(st->handlers[i].name, "loke_scheme") == 0) {
                result = st->handlers[i].cb(url_str ? url_str : "",
                                             st->handlers[i].userdata);
                break;
            }
        }
    }

    const char *body    = result ? result : "";
    TkNSUInteger blen   = strlen(body);

    id data     = ms_bytes((id)objc_getClass("NSData"),
                           "dataWithBytes:length:",
                           body, blen);
    id response = ms_url_mime_len_enc(
        ms0((id)objc_getClass("NSURLResponse"), "alloc"),
        "initWithURL:MIMEType:expectedContentLength:textEncodingName:",
        url_obj,
        ns_string("text/html"),
        (TkNSInteger)blen,
        ns_string("utf-8")
    );

    mv1(task, "didReceiveResponse:", response);
    mv1(task, "didReceiveData:", data);
    mv0(task, "didFinish");

    if (result) free(result);
}

static void scheme_stop(id self, SEL cmd, id webview, id task)
{
    (void)self; (void)cmd; (void)webview; (void)task;
}

static Class tk_scheme_handler_class(void)
{
    static Class cls = NULL;
    if (cls) return cls;

    cls = objc_allocateClassPair(objc_getClass("NSObject"),
                                  "TkLokeSchemeHandler", 0);
    if (!cls) return NULL;

    class_addIvar(cls, "state", sizeof(void *),
                  (uint8_t)__builtin_ctz(sizeof(void *)), "^v");

    Protocol *sp = objc_getProtocol("WKURLSchemeHandler");
    if (sp) class_addProtocol(cls, sp);

    class_addMethod(cls,
        sel_getUid("webView:startURLSchemeTask:"),
        (IMP)scheme_start, "v@:@@");
    class_addMethod(cls,
        sel_getUid("webView:stopURLSchemeTask:"),
        (IMP)scheme_stop, "v@:@@");

    objc_registerClassPair(cls);
    return cls;
}

/* ------------------------------------------------------------------
 * Recover state pointer from handle id string
 * ------------------------------------------------------------------ */
static TkWebviewState *tk_state_from_handle(TkWebviewHandle *h)
{
    if (!h || !h->id || h->id[0] == '\0') return NULL;
    void *ptr = NULL;
    sscanf(h->id, "%p", &ptr);
    return (TkWebviewState *)ptr;
}

/* ------------------------------------------------------------------
 * Public API — macOS
 * ------------------------------------------------------------------ */

int tk_webview_is_available(void)
{
    return 1;
}

TkWebviewHandle tk_webview_open(const char *url, const char *title,
                                 int32_t width, int32_t height)
{
    TkWebviewHandle handle;
    handle.id = NULL;
    if (!url || !title) return handle;

    /* Initialise NSApplication */
    id nsapp = ms0((id)objc_getClass("NSApplication"), "sharedApplication");
    mv_nsint(nsapp, "setActivationPolicy:", (TkNSInteger)0);

    TkWebviewState *st = (TkWebviewState *)calloc(1, sizeof(TkWebviewState));
    if (!st) return handle;

    /* WKWebViewConfiguration */
    st->config = ms0(ms0((id)objc_getClass("WKWebViewConfiguration"), "alloc"), "init");
    st->ucc    = ms0(st->config, "userContentController");

    /* loke:// scheme handler */
    id sh = ms0(ms0((id)tk_scheme_handler_class(), "alloc"), "init");
    object_setInstanceVariable(sh, "state", st);
    mv2(st->config, "setURLSchemeHandler:forURLScheme:", sh, ns_string("loke"));

    /* Delegate */
    st->delegate = ms0(ms0((id)tk_delegate_class(), "alloc"), "init");
    object_setInstanceVariable(st->delegate, "state", st);

    /* WKWebView */
    CGRect frame = {{0.0, 0.0}, {(CGFloat)width, (CGFloat)height}};
    st->webview = ms_rect_cfg(
        ms0((id)objc_getClass("WKWebView"), "alloc"),
        "initWithFrame:configuration:",
        frame, st->config
    );

    /* NSWindow */
    TkNSUInteger style =
        (TkNSUInteger)(1U | 2U | 4U | 8U); /* Titled | Closable | Mini | Resizable */
    st->window = ms_rect_style(
        ms0((id)objc_getClass("NSWindow"), "alloc"),
        "initWithContentRect:styleMask:backing:defer:",
        frame, style, (TkNSUInteger)2, NO
    );

    mv1(st->window, "setTitle:", ns_string(title));
    mv1(st->window, "setContentView:", st->webview);
    mv1(st->window, "setDelegate:", st->delegate);
    mv0(st->window, "center");

    /* Load initial URL */
    id ns_url = ms1((id)objc_getClass("NSURL"), "URLWithString:", ns_string(url));
    if (ns_url) {
        id req = ms1((id)objc_getClass("NSURLRequest"), "requestWithURL:", ns_url);
        mv1(st->webview, "loadRequest:", req);
    }

    mv1(st->window, "makeKeyAndOrderFront:", (id)nil);
    mv_bool(nsapp, "activateIgnoringOtherApps:", YES);

    char id_buf[32];
    snprintf(id_buf, sizeof(id_buf), "%p", (void *)st);
    handle.id = strdup(id_buf);
    return handle;
}

int tk_webview_close(TkWebviewHandle *h)
{
    TkWebviewState *st = tk_state_from_handle(h);
    if (!st) return 0;
    mv0(st->window, "close");
    free(st);
    free(h->id);
    h->id = NULL;
    return 1;
}

int tk_webview_set_title(TkWebviewHandle *h, const char *title)
{
    TkWebviewState *st = tk_state_from_handle(h);
    if (!st || !title) return 0;
    mv1(st->window, "setTitle:", ns_string(title));
    return 1;
}

void tk_webview_on_close(TkWebviewHandle *h,
                          TkWebviewCloseCb cb, void *userdata)
{
    TkWebviewState *st = tk_state_from_handle(h);
    if (!st) return;
    st->close_cb       = cb;
    st->close_userdata = userdata;
}

int tk_webview_register_handler(TkWebviewHandle *h, const char *name,
                                 TkWebviewHandlerCb cb, void *userdata)
{
    TkWebviewState *st = tk_state_from_handle(h);
    if (!st || !name || !cb) return 0;
    if (st->handler_count >= TK_WEBVIEW_MAX_HANDLERS) return 0;

    TkHandlerEntry *entry = &st->handlers[st->handler_count];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->cb       = cb;
    entry->userdata = userdata;
    st->handler_count++;

    if (strcmp(name, "loke_scheme") != 0) {
        mv2(st->ucc, "addScriptMessageHandler:name:",
            st->delegate, ns_string(name));
        tk_inject_bootstrap(st);
    }
    return 1;
}

int tk_webview_eval_js(TkWebviewHandle *h, const char *js)
{
    TkWebviewState *st = tk_state_from_handle(h);
    if (!st || !js) return 0;
    mv2(st->webview, "evaluateJavaScript:completionHandler:",
        ns_string(js), (id)nil);
    return 1;
}

void tk_webview_run_event_loop(void)
{
    id nsapp = ms0((id)objc_getClass("NSApplication"), "sharedApplication");
    mv0(nsapp, "run");
}

/* ======================================================================
 * Windows stub — reserved for future WebView2 integration
 * ====================================================================== */
#elif defined(_WIN32)

int tk_webview_is_available(void) { return 0; }

TkWebviewHandle tk_webview_open(const char *url, const char *title,
                                 int32_t width, int32_t height)
{
    (void)url; (void)title; (void)width; (void)height;
    TkWebviewHandle h;
    h.id = NULL;
    return h;
}

int  tk_webview_close(TkWebviewHandle *h) { (void)h; return 0; }
int  tk_webview_set_title(TkWebviewHandle *h, const char *t) { (void)h; (void)t; return 0; }
void tk_webview_on_close(TkWebviewHandle *h, TkWebviewCloseCb cb, void *ud) { (void)h; (void)cb; (void)ud; }
int  tk_webview_register_handler(TkWebviewHandle *h, const char *n, TkWebviewHandlerCb cb, void *ud) { (void)h; (void)n; (void)cb; (void)ud; return 0; }
int  tk_webview_eval_js(TkWebviewHandle *h, const char *js) { (void)h; (void)js; return 0; }
void tk_webview_run_event_loop(void) {}

/* ======================================================================
 * All other platforms — safe no-op stubs
 * ====================================================================== */
#else

int tk_webview_is_available(void) { return 0; }

TkWebviewHandle tk_webview_open(const char *url, const char *title,
                                 int32_t width, int32_t height)
{
    (void)url; (void)title; (void)width; (void)height;
    TkWebviewHandle h;
    h.id = NULL;
    return h;
}

int  tk_webview_close(TkWebviewHandle *h) { (void)h; return 0; }
int  tk_webview_set_title(TkWebviewHandle *h, const char *t) { (void)h; (void)t; return 0; }
void tk_webview_on_close(TkWebviewHandle *h, TkWebviewCloseCb cb, void *ud) { (void)h; (void)cb; (void)ud; }
int  tk_webview_register_handler(TkWebviewHandle *h, const char *n, TkWebviewHandlerCb cb, void *ud) { (void)h; (void)n; (void)cb; (void)ud; return 0; }
int  tk_webview_eval_js(TkWebviewHandle *h, const char *js) { (void)h; (void)js; return 0; }
void tk_webview_run_event_loop(void) {}

#endif /* platform */
