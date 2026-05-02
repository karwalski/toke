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
| pool_size | u64 | Maximum number of pooled connections |
| timeout_ms | u64 | Per-request timeout in milliseconds |

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

Starts an HTTP server using a pre-fork worker pool. Passing `workers=1` is equivalent to `http.serve` and runs in a single process; values greater than one fork that many worker processes before accepting connections, which improves throughput on multi-core systems.

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

Serves files from the local directory `dir` under the URL path prefix `prefix`. Any request to a path beginning with `prefix` resolves the remainder against the filesystem root `dir`. Useful for serving static assets such as CSS, images, or JavaScript.

```toke
i=http:std.http;
f=main():void{
  http.servedir("/static";"./public");
};
```

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
  let chunk=http.streamnext(s)|{$ok:v v;$err:e @()};
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
  let cert=env.get_or("TLS_CERT";"");
  let key=env.get_or("TLS_KEY";"");

  log.openaccess("logs/access.log";10000;30;0);
  log.info("server starting";@(@("port";"8080")));

  http.getstatic("/"; "<html><body><h1>Welcome</h1></body></html>");
  http.getstatic("/health"; "ok");
  http.servedir("/static";"./public");

  if(!(cert="")){
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
