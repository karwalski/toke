/* SPDX-License-Identifier: MIT */
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
 * Bridge trust policy — platform-independent (story 122.11)
 *
 * Before 122.11 the bridge honoured every caller: window.toke handlers were
 * injected into every frame (forMainFrameOnly:NO) and the message handler
 * never looked at who sent the message, so any page the view reached -- a
 * redirect target, a clicked link, a third-party iframe -- could call every
 * native handler; tk_webview_eval_js ran into whatever page was loaded; and
 * a consumer-specific URL scheme handler answered requests initiated by any
 * page (that handler was withdrawn by 136.45).  The policy
 * lives here, outside the __APPLE__ block, so it compiles and is unit-tested
 * on every host (test/conform/C139_webview_bridge_policy.sh).
 * ====================================================================== */

static int tkwv_lower_copy(char *dst, size_t cap, const char *s, size_t n)
{
    if (n + 1 > cap) return 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i];
        dst[i] = (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
    }
    dst[n] = '\0';
    return 1;
}

/* default port for a scheme, or -1 if the scheme has none we normalise */
static int tkwv_default_port(const char *scheme)
{
    if (!strcmp(scheme, "http")  || !strcmp(scheme, "ws"))  return 80;
    if (!strcmp(scheme, "https") || !strcmp(scheme, "wss")) return 443;
    return -1;
}

int tk_webview_origin_parse(const char *url, TkWebviewOrigin *out)
{
    if (!url || !out) return 0;
    memset(out, 0, sizeof *out);

    /* scheme = ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" */
    const char *p = url;
    if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z'))) return 0;
    while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
           (*p >= '0' && *p <= '9') || *p == '+' || *p == '-' || *p == '.')
        p++;
    if (*p != ':') return 0;
    if (!tkwv_lower_copy(out->scheme, sizeof out->scheme, url, (size_t)(p - url)))
        return 0;
    p++;
    if (p[0] != '/' || p[1] != '/') {
        /* no authority (about:blank, data:, ...): host stays "" */
        return 1;
    }
    p += 2;

    /* authority ends at '/', '?', '#' or end of string */
    const char *a = p;
    while (*p && *p != '/' && *p != '?' && *p != '#') p++;
    const char *aend = p;

    /* strip userinfo: it ends at the LAST '@' of the authority, so
     * "https://trusted.example@evil.example" is evil.example. */
    for (const char *q = a; q < aend; q++)
        if (*q == '@') a = q + 1;

    const char *hend = aend;
    const char *port_s = NULL;
    if (a < aend && *a == '[') {                     /* IPv6 literal */
        const char *rb = memchr(a, ']', (size_t)(aend - a));
        if (!rb) return 0;
        hend = rb + 1;
        if (hend < aend) {
            if (*hend != ':') return 0;
            port_s = hend + 1;
        }
    } else {
        for (const char *q = a; q < aend; q++)
            if (*q == ':') { hend = q; port_s = q + 1; break; }
    }
    if (!tkwv_lower_copy(out->host, sizeof out->host, a, (size_t)(hend - a)))
        return 0;

    if (port_s) {
        long port = 0;
        if (port_s == aend) port = -1;               /* "host:" = default */
        for (const char *q = port_s; q < aend; q++) {
            if (*q < '0' || *q > '9') return 0;
            port = port * 10 + (*q - '0');
            if (port > 65535) return 0;
        }
        out->port = port < 0 ? 0 : (int)port;
    }
    if (out->port != 0 && out->port == tkwv_default_port(out->scheme))
        out->port = 0;
    return 1;
}

int tk_webview_origin_trusted(const TkWebviewOrigin *o,
                              const TkWebviewOrigin *allow, int nallow)
{
    if (!o || o->scheme[0] == '\0') return 0;
    /* 136.45: a `loke:` origin was trusted here too -- one downstream
     * project's URL scheme hard-coded into the standard library.  Withdrawn
     * with the scheme handler it served; `file:` is the only built-in. */
    if (!strcmp(o->scheme, "file")) return 1;
    if (o->host[0] == '\0') return 0;                /* about:, data:, blob: */
    for (int i = 0; allow && i < nallow; i++) {
        if (!strcmp(o->scheme, allow[i].scheme) &&
            !strcmp(o->host, allow[i].host) &&
            o->port == allow[i].port)
            return 1;
    }
    return 0;
}

int tk_webview_handler_name_ok(const char *name)
{
    if (!name) return 0;
    if (!((*name >= 'a' && *name <= 'z') || (*name >= 'A' && *name <= 'Z')))
        return 0;
    size_t n = 0;
    for (const char *p = name; *p; p++, n++) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') ||
              (*p >= '0' && *p <= '9')))
            return 0;
    }
    return n <= 64;
}

char *tk_webview_js_escape(const char *s)
{
    if (!s) s = "";
    /* worst case: every byte becomes a 6-byte \uXXXX escape */
    size_t n = strlen(s);
    if (n > (((size_t)-1) - 1) / 6) return NULL;
    char *out = malloc(n * 6 + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        if (c == 0xE2 && i + 2 < n && (unsigned char)s[i + 1] == 0x80 &&
            ((unsigned char)s[i + 2] == 0xA8 || (unsigned char)s[i + 2] == 0xA9)) {
            /* U+2028 / U+2029 end a line inside a JS string literal */
            o += (size_t)snprintf(out + o, 7, "\\u%04x",
                                 (unsigned char)s[i + 2] == 0xA8 ? 0x2028u : 0x2029u);
            i += 2;
        } else if (c == '"' || c == '\\') {
            out[o++] = '\\';
            out[o++] = (char)c;
        } else if (c < 0x20 || c == 0x7F || c == '<' || c == '>') {
            o += (size_t)snprintf(out + o, 7, "\\u%04x", (unsigned)c);
        } else {
            out[o++] = (char)c;
        }
    }
    out[o] = '\0';
    return out;
}

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

    TkWebviewOrigin     allow[TK_WEBVIEW_MAX_ORIGINS];   /* 122.11 */
    int                 allow_count;
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
    /* 122.11: sized, not a fixed 16 KiB stack buffer.  The old
     * `pos += snprintf(buf+pos, sizeof buf - pos, ...)` loop ran past the
     * buffer once enough handlers were registered: pos exceeded the size,
     * the size_t subtraction wrapped, and snprintf wrote off the stack.
     * Names are validated identifiers (tk_webview_handler_name_ok), so they
     * are safe to splice into the script. */
    static const char head[] = "(function(){\n  if(!window.toke) window.toke={};\n";
    static const char tail[] = "})();\n";
    static const char per[] =
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
        "  };\n";
    size_t cap = sizeof head + sizeof tail;
    for (int i = 0; i < st->handler_count; i++)
        cap += sizeof per + 3 * strlen(st->handlers[i].name);
    char *buf = malloc(cap);
    if (!buf) return;
    size_t pos = (size_t)snprintf(buf, cap, "%s", head);

    for (int i = 0; i < st->handler_count; i++) {
        pos += (size_t)snprintf(buf + pos, cap - pos, per,
            st->handlers[i].name,
            st->handlers[i].name,
            st->handlers[i].name);
    }
    snprintf(buf + pos, cap - pos, "%s", tail);

    /* Remove existing user scripts */
    mv0(st->ucc, "removeAllUserScripts");

    /* Build WKUserScript(source:injectionTime:forMainFrameOnly:) */
    id src    = ns_string(buf);
    free(buf);
    /* forMainFrameOnly:YES (122.11) -- sub-frames never get window.toke.
     * That is hygiene, not the gate: WebKit exposes messageHandlers to
     * every frame, so delegate_did_receive_message checks the sender. */
    id script = ms_src_time_main(
        ms0((id)objc_getClass("WKUserScript"), "alloc"),
        "initWithSource:injectionTime:forMainFrameOnly:",
        src,
        (TkNSInteger)0,   /* WKUserScriptInjectionTimeAtDocumentStart */
        YES
    );
    mv1(st->ucc, "addUserScript:", script);
}

/* ------------------------------------------------------------------
 * 122.11 trust checks against live WebKit objects
 * ------------------------------------------------------------------ */

typedef BOOL        (*ObjcBool0)(id, SEL);
typedef TkNSInteger (*ObjcNSInt0)(id, SEL);

/* origin of an NSURL (nil -> untrusted) */
static int tk_nsurl_trusted(TkWebviewState *st, id url)
{
    if (!url) return 0;
    id abs = ms0(url, "absoluteString");
    const char *s = abs ? (const char *)ms0(abs, "UTF8String") : NULL;
    TkWebviewOrigin o;
    if (!s || !tk_webview_origin_parse(s, &o)) return 0;
    return tk_webview_origin_trusted(&o, st->allow, st->allow_count);
}

/* is the document currently loaded in the view trusted? */
static int tk_page_trusted(TkWebviewState *st)
{
    return st && st->webview && tk_nsurl_trusted(st, ms0(st->webview, "URL"));
}

/* did a script message come from the main frame of a trusted origin? */
static int tk_message_trusted(TkWebviewState *st, id message)
{
    id fi = ms0(message, "frameInfo");
    if (!fi) return 0;
    if (!((ObjcBool0)objc_msgSend)(fi, sel_getUid("isMainFrame"))) return 0;
    id so = ms0(fi, "securityOrigin");
    if (!so) return 0;
    id proto = ms0(so, "protocol");
    id host  = ms0(so, "host");
    const char *ps = proto ? (const char *)ms0(proto, "UTF8String") : NULL;
    const char *hs = host  ? (const char *)ms0(host,  "UTF8String") : "";
    TkNSInteger port = ((ObjcNSInt0)objc_msgSend)(so, sel_getUid("port"));
    if (!ps || !hs || port < 0 || port > 65535) return 0;
    char url[400];
    int n = port ? snprintf(url, sizeof url, "%s://%s:%ld", ps, hs, (long)port)
                 : snprintf(url, sizeof url, "%s://%s", ps, hs);
    if (n < 0 || (size_t)n >= sizeof url) return 0;
    TkWebviewOrigin o;
    if (!tk_webview_origin_parse(url, &o)) return 0;
    /* the page itself must still be trusted: the frame's origin is the
     * sender, the page URL is where the reply will be evaluated */
    return tk_webview_origin_trusted(&o, st->allow, st->allow_count) &&
           tk_page_trusted(st);
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

    /* 122.11: drop a message from a sub-frame or an untrusted origin
     * before any handler runs. */
    if (!tk_message_trusted(st, message)) return;

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
                /* 122.11: every control char, quote, backslash, '<', '>' and
                 * U+2028/9 escaped, and never truncated (the old 8 KiB
                 * buffer cut the reply mid-escape; a newline in a reply was
                 * a JS syntax error). */
                char *escaped = tk_webview_js_escape(result);
                static const char fmt[] =
                    "(function(){"
                    "var e=new CustomEvent('_toke_reply_%s',{detail:\"%s\"});"
                    "window.dispatchEvent(e);"
                    "})()";
                size_t jcap = escaped ? sizeof fmt + strlen(handler_name) + strlen(escaped) : 0;
                char *js = escaped ? malloc(jcap) : NULL;
                if (js && tk_page_trusted(st)) {
                    snprintf(js, jcap, fmt, handler_name, escaped);
                    mv2(st->webview, "evaluateJavaScript:completionHandler:",
                        ns_string(js), (id)nil);
                }
                free(js);
                free(escaped);
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

/* 136.45: the `loke://` URL scheme handler (WKURLSchemeHandler class
 * TkLokeSchemeHandler, served by a handler registered under the reserved name
 * "loke_scheme") was withdrawn.  It hard-coded one downstream project's
 * identity into the standard library, in a module already withdrawn by 136.2.
 * A consumer that needs a custom scheme restores it under its own name, in
 * its own code. */

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
    if (!tk_webview_handler_name_ok(name)) return 0;   /* 122.11: spliced into JS */
    if (st->handler_count >= TK_WEBVIEW_MAX_HANDLERS) return 0;

    TkHandlerEntry *entry = &st->handlers[st->handler_count];
    strncpy(entry->name, name, sizeof(entry->name) - 1);
    entry->name[sizeof(entry->name) - 1] = '\0';
    entry->cb       = cb;
    entry->userdata = userdata;
    st->handler_count++;

    mv2(st->ucc, "addScriptMessageHandler:name:",
        st->delegate, ns_string(name));
    tk_inject_bootstrap(st);
    return 1;
}

int tk_webview_eval_js(TkWebviewHandle *h, const char *js)
{
    TkWebviewState *st = tk_state_from_handle(h);
    if (!st || !js) return 0;
    /* 122.11: never evaluate into a page the app does not trust -- after a
     * redirect or link the view may hold a remote document. */
    if (!tk_page_trusted(st)) return 0;
    mv2(st->webview, "evaluateJavaScript:completionHandler:",
        ns_string(js), (id)nil);
    return 1;
}

int tk_webview_allow_origin(TkWebviewHandle *h, const char *origin)
{
    TkWebviewState *st = tk_state_from_handle(h);
    if (!st || !origin) return 0;
    if (st->allow_count >= TK_WEBVIEW_MAX_ORIGINS) return 0;
    TkWebviewOrigin o;
    if (!tk_webview_origin_parse(origin, &o) || o.host[0] == '\0') return 0;
    st->allow[st->allow_count++] = o;
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
int  tk_webview_allow_origin(TkWebviewHandle *h, const char *o) { (void)h; (void)o; return 0; }

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
int  tk_webview_allow_origin(TkWebviewHandle *h, const char *o) { (void)h; (void)o; return 0; }

#endif /* platform */
