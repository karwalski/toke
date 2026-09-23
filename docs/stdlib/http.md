---
title: std.http
slug: http
section: reference/stdlib
order: 17
---

**Status: Implemented** -- C runtime backing.

The `std.http` module provides both an HTTP server framework and an HTTP client. Server handlers receive a `$req` and return a `$res`. Client functions accept an `$httpclient` handle and return an `$httpresp`. Response constructor functions simplify building common response shapes.

## Types

### $req

Represents an incoming HTTP request received by a server route handler.

| Field | Type | Meaning |
|-------|------|---------|
| method | $str | HTTP method (`"GET"`, `"POST"`, `"PUT"`, `"DELETE"`, `"PATCH"`) |
| path | $str | Request path (e.g., `"/items/42"`) |
| headers | @(@($str)) | Key-value pairs of request headers |
| body | $str | Request body (empty string if none) |
| params | @(@($str)) | URL parameters extracted from the route pattern |

### $res

Represents an outgoing HTTP response returned by a server route handler.

| Field | Type | Meaning |
|-------|------|---------|
| status | u16 | HTTP status code |
| headers | @(@($str)) | Key-value pairs of response headers |
| body | $str | Response body |

### $httpclient

An opaque client handle created by `http.client`. Holds connection pool state and applies a shared timeout to all requests issued through it.

| Field | Type | Meaning |
|-------|------|---------|
| baseurl | $str | Base URL prepended to all request paths |
| poolsize | u64 | Maximum number of pooled connections |
| timeoutms | u64 | Per-request timeout in milliseconds |

### $httpresp

Represents an HTTP response returned by a client request function.

| Field | Type | Meaning |
|-------|------|---------|
| status | u64 | HTTP status code |
| headers | @(@($str)) | Key-value pairs of response headers |
| body | @(byte) | Response body as a raw byte array |

### $httpstream

An opaque handle representing an open streaming HTTP connection, obtained from `http.stream`.

| Field | Type | Meaning |
|-------|------|---------|
| id | u64 | Internal stream identifier |
| open | bool | `true` while the connection is still live |

## Server Route Registration

Route registration functions bind a URL pattern to a handler function. Handlers are registered before calling `http.serve` or `http.serveworkers`; the server matches incoming requests to registered patterns in registration order.

### http.GET(pattern: $str; handler: f): void

Registers `handler` to be called for all `GET` requests matching `pattern`. Patterns may contain named segments (e.g., `"/users/:id"`), which are made available via `http.param`.

```text
http.GET("/health"; handler);
```

### http.POST(pattern: $str; handler: f): void

Registers `handler` for all `POST` requests matching `pattern`. The request body is available as `req.body`.

```text
http.POST("/items"; handler);
```

### http.PUT(pattern: $str; handler: f): void

Registers `handler` for all `PUT` requests matching `pattern`. Commonly used for full-resource replacement endpoints.

```text
http.PUT("/items/:id"; handler);
```

### http.DELETE(pattern: $str; handler: f): void

Registers `handler` for all `DELETE` requests matching `pattern`. The request body is typically empty.

```text
http.DELETE("/items/:id"; handler);
```

### http.PATCH(pattern: $str; handler: f): void

Registers `handler` for all `PATCH` requests matching `pattern`. Use for partial-update endpoints.

```text
http.PATCH("/items/:id"; handler);
```

## Server Start Functions

### http.serve(port: u16): void

Starts an HTTP server on the given port, blocking the current process until the server is shut down. Route handlers registered via `http.GET`, `http.POST`, etc. are dispatched automatically.

```toke
m=example;
i=http:std.http;
f=main():i64{
  http.getstatic("/"; "hello");
  http.serve(8080);
  <0;
};
```

### http.serveworkers(port: u64; workers: u64): void

Starts an HTTP server using a pre-fork worker pool. Passing `workers=1` runs in a single process with no fork at all; values greater than one fork that many worker processes before accepting connections, which improves throughput on multi-core systems. Passing `workers=0` auto-detects the CPU count, overridable with the `TK_HTTP_WORKERS` environment variable.

`http.serve(port)` is `http.serveworkers(port;2)`, not the single-process path — so a measurement taken against `http.serve` is a measurement of two processes, and `http.serveworkers(port;1)` is the one to use when you need a single PID to watch.

```toke
i=http:std.http;
f=main():void{
  http.serveworkers(8080;4);
};
```

### http.servetls(port: u64; cert_path: $str; key_path: $str): void

Starts an HTTPS server with TLS termination, loading the certificate and private key from the given PEM file paths. The server blocks until shut down. Use `http.serveworkers` with TLS configuration for multi-worker HTTPS deployments.

```toke
i=http:std.http;
f=main():void{
  http.servetls(443;"/etc/certs/server.crt";"/etc/certs/server.key");
};
```

## Abandoned requests

A client that sends a request and goes away before reading the answer is
ordinary browser behaviour: the browser has read part of a response, the user
navigates away, the socket closes with data still queued, and the server's
next write finds the connection reset.

Until story 127.144 that killed the server. Nothing in the runtime suppressed
`SIGPIPE`, whose default disposition terminates the process, so a single
abandoned request exited `http.serveworkers(port;1)` and `http.servetls` with
status 141, and cost `http.serve` a worker — which its supervisor then
respawned, once per abandoned request. No handler code was involved and no
configuration avoided it.

Every write to a client socket now goes through one funnel that suppresses the
signal per socket (`SO_NOSIGPIPE`, on macOS and the BSDs) and per send
(`MSG_NOSIGNAL`, on Linux), and reports the broken connection as an ordinary
error instead. `test/conform/S007_http_abandoned_request.sh` drives all three
serve paths, because all three used to fail differently.

### What servetls does to SIGPIPE on Linux

One gap is not closable per socket or per send: OpenSSL writes the TLS record
to the socket from inside its own BIO, using `write(2)`, so `MSG_NOSIGNAL`
never reaches that call — and Linux has no `SO_NOSIGPIPE` to set on the socket
instead. On a platform in that position, and only there, `http.servetls`
installs `SIG_IGN` for `SIGPIPE` when it starts.

**This is process-wide, and it is worth knowing about.** After
`http.servetls` is called, a write to any broken pipe in that process returns
`EPIPE` instead of terminating it, and because an *ignored* disposition
survives `execve` as well as `fork`, any child process the server spawns
inherits it too. A handler that shells out to a pipeline — the classic
`… | head` shape, which relies on `SIGPIPE` to stop the producer — will see
the difference.

It is scoped as tightly as it can be: it is installed inside `http.servetls`,
which never returns, so no program that has not committed to being a TLS
server is affected, and on macOS and the BSDs it is not installed at all.

## Rate limiting

**`std.http` does not rate limit, and deliberately so.** Put rate limiting in
front of the server — in your reverse proxy, CDN or load balancer — where the
client identity is actually known.

Until story 127.60 the server carried a built-in per-IP limiter: 200 per 60
seconds, as two compile-time constants with no setting, no environment
variable and no mention on this page. It was removed rather than tuned,
because it was wrong in both directions at once.

It did not throttle. The check ran once per *connection*, not per request, so
a client on a single keep-alive socket was never limited at all — 886 of 900
requests served, no `429` — while 199 separate connections from the same
address tripped it. And a bucket was reset whenever the address in it differed
from the caller's, so any two addresses colliding in the 1024-bucket table
wiped each other's window on every request and neither was ever limited.

It did throttle the wrong traffic. Client identity came only from the socket's
peer address; there was no `X-Forwarded-For` handling. Behind a proxy or a CDN
— the normal deployment — every client in the world shared one bucket and the
whole site was capped at 200 connections a minute.

And the number was not 200. `http.serve` and `http.serveworkers` pre-fork
worker processes, each with its own copy of the table, so the real cap was 200
times the worker count, decided by whichever worker accepted: measured at 400
with the two workers `http.serve` uses. HTTP/2 over TLS skipped the check
entirely.

Doing it properly is not a matter of exposing the two constants. It needs
state shared across the pre-fork workers, a table that does not evict on
collision, per-request counting, and a **trusted-proxy policy** for deciding
which address to count — reading `X-Forwarded-For` without one replaces a
shared-bucket bug with a one-header total bypass, which is worse than nothing.
That is a subsystem to be specified and designed, not a default to be left in
place because it exists.

### http.getstatic(path: $str; html: $str): void

Registers a static HTML response for the given path. When the server receives a request matching `path`, it returns the pre-rendered `html` string as a `200 OK` response with `Content-Type: text/html`. Use this for pages whose content is known at startup.

```toke
i=http:std.http;
f=main():void{
  http.getstatic("/"; "<html><body><h1>Hello</h1></body></html>");
  http.getstatic("/about"; "<html><body><p>About page</p></body></html>");
};
```

### http.servedir(prefix: $str; dir: $str): void

Mounts the local directory `dir` under the URL path prefix `prefix`. Any
request to a path beginning with `prefix` is resolved against `dir`. Useful
for serving static assets such as CSS, images, or JavaScript.

The request path is appended to `dir` **whole, including the prefix** — so
`http.servedir("/static";"./public")` serves `/static/style.css` from
`./public/static/style.css`, not from `./public/style.css`. Lay the directory
out to match.

```toke
i=http:std.http;
f=main():void{
  http.servedir("/static";"./public");
};
```

### Several mounts

`http.servedir` may be called more than once, and each call is an independent
mount:

```toke
f=main():void{
  http.servedir("/static";"./public");
  http.servedir("/media";"/var/lib/media");
  http.serve(8080);
};
```

Until story 127.75 this did not work: the prefix and root were a single pair
of globals that every call overwrote, so every mount but the **last** silently
stopped resolving — the earlier prefixes answered 404 while the last one
answered 200, with nothing logged. Mounts are now kept in a table of up to 32,
matched **longest prefix first**, so a narrower mount nested inside a broader
one wins:

```toke
  http.servedir("/static";"./public");
  http.servedir("/static/vendor";"/opt/vendor-assets");
```

Re-mounting a prefix that is already mounted replaces that mount's root rather
than adding a second entry. Mounting a 33rd distinct prefix is refused with a
message on stderr.

## Accessor Functions

### http.param(req: $req; name: $str): $str!$httperr

Extracts a named URL parameter captured by the route pattern (e.g., the `id` from `"/users/:id"`). Returns `$httperr.$notfound` if no parameter with that name was captured for this request.

```text
let id = http.param(req; "id");
-- id = ok("42") when path matched "/users/:id"
```

### http.header(req: $req; name: $str): $str!$httperr

Extracts a request header value by name using case-insensitive lookup. Returns `$httperr.$notfound` if the header is not present in the request.

```text
let ct = http.header(req; "content-type");
-- ct = ok("application/json")
```

## Response Constructors

### Who owns the response body

**A `$res` does not own its body.** All four constructors store the pointer
they are given; nothing copies it and nothing downstream frees it. That is
deliberate — it is what lets a handler answer with a literal, and it is what
makes the obvious `:param` handler work:

```toke
f=greet(req:$req):$res{
  <http.resok(http.param(req;"name"))
};
```

`http.param` hands back memory that belongs to the *request*, which outlives
the response by construction: the bindings are released only after the
response, the compression buffer and the access-log line have all been
written. Freeing them when the handler returned was tried once, under story
127.72, and every such response came back **empty**.

The rule this implies is only visible from the C side, so it is stated here
as well. A body is exactly one of:

| Kind | Example | Who releases it |
|------|---------|-----------------|
| static storage | a string literal, a MIME-type table entry | nobody; it is never freed |
| request-owned memory | `http.param`, `http.header` | the request, after the response is written |
| heap the handler allocated | a file read from disk, a rendered template | the handler, by handing the pointer to `http_defer_free()` in the same breath |

There is no fourth case. A heap body that is not deferred is **leaked** — no
code path anywhere frees a `$res` body — and until story 127.148 all three
built-in handlers that build a body on the heap did exactly that:
`http.servedir` leaked the whole file on every request, `http.servepages`
leaked every rendered page, and the vhost static handler leaked the file plus
its header block despite `router_static_serve`'s own written contract saying
the caller must free them. A 64 KiB asset cost 81,920 bytes of heap per
request; it now costs zero, measured by
`test/conform/S009_http_res_body_ownership.sh`.

This only concerns code that writes a handler in C. A toke handler returning
`http.resok(...)` is always in one of the first two rows.

### http.$res.ok(body: $str): $res

Creates a 200 OK response with the given plain-text body. Sets no explicit `Content-Type` header; add one via the `headers` field if needed.

```text
let r = http.$res.ok("hello");
-- r.status = 200; r.body = "hello"
```

### http.$res.json(status: u16; body: $str): $res

Creates a response with the given status code and body, automatically setting the `Content-Type` header to `application/json`. Use this for all JSON API responses.

```text
let r = http.$res.json(201; "{\"created\":true}");
-- r.status = 201; r.headers includes Content-Type: application/json
```

### http.$res.bad(msg: $str): $res

Creates a 400 Bad Request response with `msg` as the body. Intended for client-side input validation failures.

```text
let r = http.$res.bad("missing required field: name");
-- r.status = 400
```

### http.$res.err(msg: $str): $res

Creates a 500 Internal Server Error response with `msg` as the body. Use this when an unexpected server-side failure prevents normal response generation.

```text
let r = http.$res.err("database unavailable");
-- r.status = 500
```

## HTTP Client Functions

### http.client(baseurl: $str): $httpclient

Creates an HTTP client configured to send all requests relative to `baseurl`. The returned handle maintains a connection pool and a default timeout; pass it to `http.post`, `http.put`, `http.delete`, and other client functions.

```toke
i=http:std.http;
f=main():void{
  let c=http.client("https://api.example.com");
};
```

### http.get(url: $str): $httpresp!$httperr

Sends a `GET` request to the given URL and returns the response. Returns `$httperr` on network failure, timeout, or DNS error.

```toke
i=http:std.http;
f=main():void{
  let resp=http.get("https://api.example.com/status");
};
```

### http.post(client: $httpclient; path: $str; body: @(byte); content_type: $str): $httpresp!$httperr

Sends a `POST` request with the given byte-array body and `Content-Type` header. Use `str.bytes` to convert a string body. Returns `$httperr` on failure.

```toke
i=http:std.http;
i=str:std.str;
f=main():void{
  let c=http.client("https://api.example.com");
  let resp=http.post(c;"/items";str.bytes("{\"name\":\"toke\"}");"application/json");
};
```

### http.put(client: $httpclient; path: $str; body: @(byte); content_type: $str): $httpresp!$httperr

Sends a `PUT` request with the given byte-array body and `Content-Type` header. Semantics mirror `http.post`; use for full-resource replacement.

```toke
i=http:std.http;
i=str:std.str;
f=main():void{
  let c=http.client("https://api.example.com");
  let resp=http.put(c;"/items/1";str.bytes("{\"name\":\"updated\"}");"application/json");
};
```

### http.delete(client: $httpclient; path: $str): $httpresp!$httperr

Sends a `DELETE` request to `path`. The request carries no body. Returns `$httperr` on network failure.

```toke
i=http:std.http;
f=main():void{
  let c=http.client("https://api.example.com");
  let resp=http.delete(c;"/items/1");
};
```

### http.stream(client: $httpclient; method: $str; path: $str): $httpstream!$httperr

Opens a streaming HTTP connection for server-sent events or chunked transfer responses. Returns an `$httpstream` handle on success; use `http.streamnext` to read successive chunks. The connection remains open until `open` is `false` or an error is returned.

```toke
i=http:std.http;
f=main():void{
  let c=http.client("https://api.example.com");
  let s=http.stream(c;"GET";"/events");
};
```

### http.streamnext(stream: $httpstream): @(byte)!$httperr

Reads the next available chunk of bytes from an open `$httpstream`. Returns an empty byte array when the server closes the connection gracefully; returns `$httperr` on network or protocol error. Call this in a loop until the returned byte array is empty or an error occurs.

```toke
i=http:std.http;
f=main():void{
  let c=http.client("https://api.example.com");
  let s=http.stream(c;"GET";"/events");
  let chunk=mt http.streamnext(s) {$ok:v v;$err:e @()};
};
```

## Usage Example

The following example registers static routes, serves a directory of assets, and starts a multi-worker HTTPS server. For dynamic request handling see `std.router`.

```toke
m=example;
i=http:std.http;
i=log:std.log;
i=env:std.env;

f=main():i64{
  let port=8080;
  let cert=env.getor("TLS_CERT";"");
  let key=env.getor("TLS_KEY";"");

  log.openaccess("logs/access.log";10000;30;0);
  log.info("server starting";@(@("port";"8080")));

  http.getstatic("/"; "<html><body><h1>Welcome</h1></body></html>");
  http.getstatic("/health"; "ok");
  http.servedir("/static";"./public");

  if(!(cert=="")){
    http.servetls(443;cert;key);
  }el{
    http.serveworkers(port as u64;4);
  };
  <0;
};
```

## Error Types

### $httperr

A sum type representing HTTP operation failures, returned by accessor functions and client request functions.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $badrequest | $str | The request is malformed or the argument is invalid |
| $notfound | $str | The requested parameter, header, or resource was not found |
| $internal | $str | An internal server or client error occurred |
| $timeout | u32 | The operation timed out; value is the timeout in milliseconds |
