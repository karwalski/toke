---
title: std.router
slug: router
section: reference/stdlib
order: 31
---

**Status: Implemented** -- C runtime backing.

The `std.router` module provides a declarative HTTP router. Register routes by method and path pattern, attach middleware, and serve the application.

## Types

### $router

Opaque handle to a router instance.

### $handler

Opaque handle to a route handler function.

### $middleware

Opaque handle to a middleware function.

### $routererr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `router.new` | | `$router` | Create a new router |
| `router.get` | `r: $router; path: $str; h: $handler` | `void` | Register a GET route |
| `router.post` | `r: $router; path: $str; h: $handler` | `void` | Register a POST route |
| `router.put` | `r: $router; path: $str; h: $handler` | `void` | Register a PUT route |
| `router.delete` | `r: $router; path: $str; h: $handler` | `void` | Register a DELETE route |
| `router.use` | `r: $router; mw: $middleware` | `void` | Attach middleware to the router |
| `router.serve` | `r: $router; host: $str; port: u64` | `void!$routererr` | Start serving HTTP on `port`. Port 0 binds a free port and prints `DIAGNOSTIC: kind=server_listening port=N` on stderr; a port above 65535, or one that cannot be bound, is `$err` (135.26). `host` is the address to listen on, by `http.serveon`'s rules: an IPv4 literal, `"localhost"` (127.0.0.1), or `""`, `"*"` or `"0.0.0.0"` for all interfaces; any other host is `$err` before anything is bound (135.28) |

### A port that is already in use

`router.serve` will not start beside a live listener on the same port: if
another server already listens on `port` -- at the same address, or at
`127.0.0.1` when you asked for all interfaces, or at all interfaces when you
asked for `127.0.0.1` -- it returns `$err` ("bind() failed: address already
in use"). On macOS such a bind used to succeed and silently take that
address's traffic from the first server (135.33). A port whose previous
server has exited, with old connections still closing (TIME_WAIT), binds as
before.

**Limit:** for an all-interfaces bind (`""`, `"*"`, `"0.0.0.0"`) the check
asks `127.0.0.1` only. A server listening on one other specific address alone
(say `192.168.1.5:8080`) is not detected there: on macOS the new server
starts, and that address's traffic keeps going to the first server while
every other address's goes to the new one. On Linux the bind fails either
way.

### Response bodies are bytes

A response body leaves the router whole, 0x00 bytes included: a binary file
served by the C API's static serving (`router_static`, `router_static_serve`),
a byte range of one, and a body the gzip middleware (`router_use_gzip`) has
compressed. Their length travels in `TkRouteResp.body_len` (0 means the body
is a string, sized by its terminator), and the writer, the gzip middleware
and the ETag middleware size the body with `router_resp_body_len()`. Until
story 121.88 all of them used `strlen`, so a PNG went out as its 8-byte
signature, a gzip stream could not be read past its third byte, and two
binary files that agreed before their first 0x00 shared an ETag. A C caller
building a `TkRouteResp` by hand must zero-initialise it.

The gzip middleware leaves already-compressed content types alone: `image/*`
except `image/svg+xml`, `video/*`, `audio/*`, `font/*`,
`application/octet-stream`, `application/wasm`, `application/zip` and
`application/gzip` -- the list `std.http`'s server uses (story 121.89). Before
it, fonts, wasm and archives were recompressed and SVG was not compressed.

It compresses only for a client whose `Accept-Encoding` admits gzip under
RFC 9110 §12.5.3 -- `gzip` or `x-gzip` with a weight above zero, or, gzip
unlisted, `*` with one -- and only a response with no `Content-Encoding` of
its own (story 121.94). Before it, `gzip;q=0` was taken as a yes, and a
handler's `Content-Encoding: br` body was gzipped again under two codings.

The ETag middleware's `304 Not Modified` carries the `ETag`, and the
`Cache-Control`, `Expires`, `Content-Location`, `Date` and `Vary` fields the
`200` it stands for carried, as RFC 9110 §15.4.5 requires -- no other
header, and no body (story 121.95; it used to carry no header at all, so a
cache revalidating its copy learned neither the validator nor a new
lifetime). A middleware registered before `router_use_etag` runs outside it
and does not see the `200`: register the ETag middleware first when the
`304` should carry the gzip middleware's `Vary: Accept-Encoding`.

### Response handles

`router.ok`, `router.html`, `router.json`, `router.css`, `router.text`,
`router.bad`, `router.notfound`, `router.status` and `router.respond` return
an `i64` handle to a response, and a handler returns one. A handle made
inside a handler belongs to that request: it is released once the router
has read the handler's answer, whether the handler returned it or not. A
handle made outside any handler -- in `main`, or in a module `let` such as
`let notfound=router.notfound("gone");`, initialised before `router.serve`
-- lasts for the whole program, so it can be returned on every request.

A handle kept past its request and returned again -- one a handler caches
in a mutable global, say -- is not a response any more: the request is
answered `500` and `DIAGNOSTIC: kind=router_invalid_response` is printed on
stderr. So is any `i64` that is not a handle. Build the response per request,
or before serving. (Story 121.93: every handle used to be a heap block that
nothing freed, so a server leaked one per request and one more for each
response a handler built and did not return; a cached handle "worked" only
because it was never freed, and a stray integer crashed the server.)

## Usage

```toke
m=example;
i=router:std.router;

f=main():i64{
  let r = router.new();
  router.post(r; "/hello"; 0);
  mt router.serve(r; "0.0.0.0"; 8080) {$ok:v 0;$err:e 1};
  <0;
}
```

## Dependencies

- `std.http` -- request and response types.
