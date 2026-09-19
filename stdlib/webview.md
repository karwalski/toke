# std.webview -- WITHDRAWN (story 136.2)

> **This module is not part of the standard library. It cannot be imported and
> it never worked.** `stdlib/webview.tki` was deleted on 2026-09-19 and
> `i=wv:std.webview;` now fails at the import with `E2030 standard-library
> module 'std.webview' not found`, naming the module and the line.
>
> Everything below the horizontal rule is the 72.4 design, kept as the record
> of what a restored module should do. **Do not write against it.**

## Why it was withdrawn rather than implemented

72.4 recorded eight functions as done and "16/16 pass". No `webview_glue.c` was
ever written, so not one of the eight had a `_w` symbol, and the platform
frameworks were absent from the link line. A consumer importing it got a linker
error over `_tk_webview_*` plus eleven undefined Objective-C runtime symbols
(`_objc_msgSend`, `_sel_getUid`, `_objc_allocateClassPair`, ...). loke wrote
`packages/browser/src/shell.tk` against it and it has never linked.

Implementing it was weighed and rejected for now, on four grounds:

1. **It cannot be verified here.** `webview.open` creates an `NSWindow` and a
   `WKWebView`; `webview.runeventloop` starts the `NSApplication` run loop.
   Exercising it means putting a window on screen from an unsigned CLI process
   and loading a URL, which raises system-level prompts. Shipping it unexercised
   would repeat exactly the defect this epic exists to correct — Epic 136 is the
   record of a surface that reported 306 passing tests while four of its modules
   were unusable.
2. **Two of the eight need a language decision, not glue.** `onclose` and
   `registerhandler` take toke function references (`fn():void`, `fn(str):str`)
   and the C callbacks carry a `void *userdata` the toke side has no slot for,
   so they need a per-handle trampoline registry and a callback ABI that toke
   does not have. 131.38 already quarantined them on exactly this ground. An
   implementation of the other six is not a usable module: loke's own consumer
   calls `wv.onclose` on the line after `wv.open`.
3. **The handle type needs an ABI decision too.** `tk_webview_open` returns
   `TkWebviewHandle` by value and `tk_webview_close` takes its address and frees
   the `id` but not the struct, so the glue must own a handle table rather than
   cast a pointer.
4. **A known-unsafe surface would ship ahead of its fix.** 121.38 and 122.11 are
   open: the `WKWebView` bridge loads an arbitrary URL with JS enabled, exposes
   native `messageHandlers`, host `evaluateJavaScript:` and a custom `loke://`
   scheme, with no origin restriction. `registerhandler` is the function that
   exposes it.

The C core (`src/stdlib/webview.c`, `src/stdlib/webview.h`) is retained and
unbuilt, so restoring the module costs a `webview_glue.c`, the link flags
`-framework WebKit -framework Cocoa -lobjc`, and the two decisions above.

## Correction to the published signature

Restore it with the **requested** parameter order. loke asked for
`open(url; title; width; height)` (`docs/archive/ooke-bindings-required.md`) and
its consumer calls it that way. The deleted `.tki` agreed. The **spec**
(`docs/spec/toke-spec-v0.3.md` §16.44) had it reversed as
`open(title; url; ...)`, and also used the pre-113.2a underscore spellings
(`set_title`, `is_available`) that the `.tki` had already dropped. Two
documents, two different interfaces, neither of them callable.

---

## Overview

The `std.webview` module embeds a native browser window (backed by the platform's system web engine) that can host an ooke HTTP server or any URL. JavaScript running inside the web view communicates with toke code exclusively through handlers registered via `webview.register_handler` — arbitrary toke functions are not reachable from JS. This sandboxing is enforced by the platform message-passing layer (WKScriptMessageHandler on macOS).

The module is currently available on macOS (WKWebView). On other platforms `webview.is_available()` returns `false` and all other functions are safe no-ops.

## Types

### webview_handle

An opaque handle representing one open native browser window.

| Field | Type | Meaning |
|-------|------|---------|
| id | $str | Unique string identifier for the window |

## Functions

### webview.open(url: $str; title: $str; width: i32; height: i32): webview_handle

Opens a new native browser window loading `url`, with the given `title` and pixel dimensions. Returns a handle used with all other functions. On unsupported platforms returns a handle with an empty `id`.

**Example:**
```toke
let wv = webview.open("http://localhost:8080"; "My App"; 1024; 768);
```

### webview.close(h: webview_handle): bool

Closes the window identified by `h`. Returns `true` on success, `false` if the handle is invalid or the platform does not support webview.

**Example:**
```toke
let ok = webview.close(wv);
```

### webview.set_title(h: webview_handle; title: $str): bool

Updates the title bar text of the window. Returns `true` on success.

**Example:**
```toke
let ok = webview.set_title(wv; "Updated Title");
```

### webview.on_close(h: webview_handle; cb: fn(): void): void

Registers a callback that is invoked when the user closes the window. The callback receives no arguments. Replaces any previously registered close callback for this handle.

**Example:**
```toke
webview.on_close(wv; fn() { log.info("window closed") });
```

### webview.register_handler(h: webview_handle; name: $str; cb: fn($str): $str): bool

Registers a named handler callable from JavaScript as `window.toke.{name}(data)`. When called from JS, the native side invokes `cb` with the JSON `data` string and posts the returned string back to JS as the resolved value.

Only handlers explicitly registered this way are reachable from JavaScript. There is no mechanism for JS to call arbitrary toke functions.

Returns `true` on success.

**Example:**
```toke
let ok = webview.register_handler(wv; "greet"; fn(data: $str): $str {
    "Hello, " ++ data
});
```

Inside the web view:
```js
const reply = await window.toke.greet("world");
// reply == "Hello, world"
```

### webview.eval_js(h: webview_handle; js: $str): bool

Evaluates a JavaScript string in the context of the current page. Returns `true` if the evaluation was dispatched successfully (not necessarily that the script ran without error).

**Example:**
```toke
let ok = webview.eval_js(wv; "document.title = 'changed';");
```

### webview.run_event_loop(): void

Runs the platform event loop, blocking the calling thread until all webview windows have been closed. On macOS this runs the NSApplication main run loop. Call this after opening all windows.

**Example:**
```toke
let wv = webview.open("http://localhost:8080"; "Demo"; 800; 600);
webview.run_event_loop();
```

### webview.is_available(): bool

Returns `true` if the webview module is supported on the current platform. Always call this before opening a window when portability matters.

**Example:**
```toke
if webview.is_available() {
    let wv = webview.open("http://localhost:8080"; "App"; 1024; 768);
    webview.run_event_loop();
} else {
    log.warn("webview not available on this platform");
}
```

## URL Scheme: loke://

The module registers a custom `loke://` URL scheme handler when a window is opened. Requests to `loke://` URLs are routed to the registered handler named `"loke_scheme"` if present, enabling deep-link navigation from within the web view back to toke application logic.

## Platform Notes

| Platform | Backend         | Status |
|----------|-----------------|--------|
| macOS    | WKWebView       | Supported |
| Windows  | WebView2 (stub) | Planned |
| Other    | —               | `is_available()` returns `false` |

The C implementation file must be compiled as Objective-C (`.m` or `-x objective-c`) on macOS because it uses the Cocoa and WebKit frameworks. See `webview.h` for details.
