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
