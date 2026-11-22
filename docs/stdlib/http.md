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

Every field can be read directly, `req.body`, `req.headers.len`, `req.params.get(0).get(1)`, as well as through the accessor functions below. Each element of `headers` and `params` is a two-element `@(name; value)`; header names keep the case the client sent, so compare them case-insensitively (`http.header` and `http.reqheader` do). A `$req` and every string in it belong to the request: they are released after the response is sent, so a handler that keeps one past its return keeps dangling strings.

Until story 138.18 a field read went wrong: the server handed a handler its internal C request, whose fields are in a different order and whose pair lists are a different shape, so `req.body` read the header list and `req.headers.len` crashed. The accessor functions were not affected.

### $res

Represents an outgoing HTTP response returned by a server route handler.

| Field | Type | Meaning |
|-------|------|---------|
| status | u16 | HTTP status code |
| headers | @(@($str)) | Key-value pairs of response headers |
| body | $str | Response body |

A handler may build its response as a literal, `$res{status:201;headers:@(@("Location";"/items/7"));body:""}`, and the server sends that status, those headers and that body, framed by the server (below). The fields of a `$res` from a constructor (`http.res.ok`, `http.resnew`, ...) read back what was passed, `r.status` and `r.body`, with the constructor's `Content-Type` in `r.headers` (story 138.18).

**A header that could split the response is not sent** (story 121.92). A header whose name is not an HTTP token (letters, digits and ``!#$%&'*+-.^_`|~``; no space, colon, carriage return or line feed) or whose value holds a carriage return, a line feed or another control character other than tab is dropped whole, over HTTP/1.1, TLS and HTTP/2 alike, and the rest of the response is sent; the server does not repair it, since stripping bytes would still send a value nobody wrote. A handler that copies request data into a header, such as `@("Location"; req.body)`, therefore cannot be made to write a header line of the client's choosing. A dropped `X-Frame-Options`, `X-Content-Type-Options` or `Referrer-Policy` does not displace the server's default for it. A header is sent at its full length; until 121.92 a header line longer than 511 bytes was cut, and the header after it was joined onto it. Until story 121.98 HTTP/2 dropped only a value holding a carriage return or line feed, so a value with another control character was sent over HTTP/2 and dropped over HTTP/1.1.

**The server frames the response** (story 121.97). `Content-Length`, `Transfer-Encoding` and `Connection` are the server's own: it computes them from the body it sends and the connection it keeps, and a handler's copy of any of them is not sent. A handler's `Content-Length` still chooses how an HTTP/1.1 body is framed — with it the body is sent with a fixed length, without it in chunks — but the length sent is always the body's. A `1xx`, `204` or `304` response is sent with no body and neither field, whatever body the handler gave it, over HTTP/1.1, TLS and HTTP/2, and in answer to `HEAD`. Until then a handler's `Content-Length` was sent after the server's, so one that differed from the body gave the response two lengths, and a client or proxy could frame it by either (the request-smuggling class, RFC 9112 §6.3); a `204` carried `Content-Length: 0`, and a `204` or `304` with a body sent the body, which a client reads as the start of the next response. A `HEAD` response carries the head the `GET` for the same resource would (story 121.100, RFC 9110 §9.3.2): the same `Content-Length` — the gzip length when the `GET` is sent gzip-encoded — or the same `Transfer-Encoding: chunked`, and the same `Content-Encoding` and `Vary`, over HTTP/1.1, TLS and HTTP/2; the server encodes the body to learn the length, then sends no body. Until then a `HEAD` gave the uncompressed length and no coding where the `GET` was gzipped, and a fixed length where the `GET` was chunked.

**A handler can close the connection** (story 121.101). The server's `Connection` is its own, but a handler's `Connection` field that lists `close` (`close`, `Keep-Alive, close`, any case) is honoured: the response goes out with `Connection: close`, nothing further is read or served on that connection — a request the client pipelined behind it is not answered — and the server half-closes the socket and drains what the client is still sending before it closes, so the response is not lost to a TCP reset. A handler's `keep-alive` changes nothing; the server still decides when to close (when the client asks, or at its per-connection request limit). A client's `Connection` fields are read as lists too, so `Connection: upgrade, close` closes like `Connection: close` (RFC 9110 §7.6.1). This holds on HTTP/1.1 and TLS; HTTP/2 has no `Connection` field (RFC 9113 §8.2.2) and drops a handler's. Until then a handler's `close` was not sent and the connection stayed open, and a client's `close` was seen only as the first option of the first `Connection` field.

**An HTTP/1.0 request closes the connection unless it asks for keep-alive** (story 121.102, RFC 9112 §9.3). HTTP/1.1 keeps a connection open by default; HTTP/1.0 does not, so after the response to a 1.0 request the server sends `Connection: close` and closes — unless the request's `Connection` fields list `keep-alive` (any case, `x-foo, Keep-Alive` too), in which case the response says `Connection: keep-alive` and the next request is read. `close` in the list still wins. The version is read from the request line, plain and TLS. Until then a 1.0 request was kept like a 1.1 one: the response said `keep-alive`, a request pipelined behind it was served, and a 1.0 client reading the response to the end of the connection waited for the 2-second idle timeout.

**An HTTP/1.0 client is never sent `Transfer-Encoding: chunked`** (story 121.103, RFC 9112 §6.1). The chunked coding is HTTP/1.1; a 1.0 client reads the chunk sizes and the final `0` as part of the body. A response to an HTTP/1.0 request — the handler's, a `HEAD`'s, or the server's own `405` — is sent with a `Content-Length`, whether or not the handler gave one; the body is complete when the handler returns, so nothing has to be delimited by closing the connection. The server's answers to a request it could not frame (`400`, `413`, `431`, ...) are sent with a length on every version, since that request's version may not be known. HTTP/1.1 is unchanged: a body the handler gave no `Content-Length` is sent in chunks on the plain server, and the TLS server always sends a length. Until then a 1.0 request got the same chunked framing as a 1.1 one.

### $httpclient

An opaque client handle created by `http.client`. Holds connection pool state and applies a shared per-read timeout to all requests issued through it.

| Field | Type | Meaning |
|-------|------|---------|
| baseurl | $str | Base URL prepended to all request paths |
| poolsize | u64 | Maximum number of pooled connections |
| timeoutms | u64 | Per-read timeout in milliseconds (30000): how long each wait for data may take |

**A client written as a record literal** — `$httpclient{baseurl:"http://h";poolsize:4;timeoutms:5000}` — is a client with those three fields, no proxy and none of the [request controls](#request-controls). The proxy and the controls are not record fields, so only a client from `http.client`, or from `http.withproxy` and the other `with` options, carries them. Each `with` option accepts a literal client and returns a client with the one setting added (story 138.24: the client used to read these settings from past the end of a literal, so a literal client took a spurious cap or deadline, and `http.withproxy` on one could crash).

**`timeoutms` is per read, not per request** (story 138.34). It is the socket's receive timeout (`SO_RCVTIMEO`), which restarts whenever data arrives, so a server that sends a byte inside every timeout keeps a request open indefinitely. Until 138.34 this page called it a per-request timeout. To bound a request as a whole, use [`http.withdeadline`](#httpwithdeadlineclient-httpclient-ms-u64-httpclient). To bound the body a response may carry, use [`http.withmaxbody`](#httpwithmaxbodyclient-httpclient-bytes-u64-httpclient). Both are off on a client from `http.client`.

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

Route registration functions bind a URL pattern to a handler function. The names are lower case, as `stdlib/http.tki` declares them: upper case is outside toke's character set, so `http.GET` is rejected at parse time (E2002). The handler, typed `funcdecl` in the interface, is a top-level function passed by reference, `&handler`. These five names are route registration only; the client calls are `http.clientget` and the rest (see [Client calls and route registration are different names](#client-calls-and-route-registration-are-different-names)). Handlers are registered before calling `http.serve` or `http.serveworkers`; the server matches incoming requests to registered patterns in registration order.

### http.get(pattern: $str; handler: funcdecl): void

Registers `handler` to be called for all `GET` requests matching `pattern`. Patterns may contain named segments (e.g., `"/users/:id"`), which are made available via `http.param`.

```text
http.get("/health"; &handler);
```

### http.post(pattern: $str; handler: funcdecl): void

Registers `handler` for all `POST` requests matching `pattern`. The handler reads the request body as the field `req.body` or with `http.reqbody(req)`; both read the same value (story 138.18).

```text
http.post("/items"; &handler);
```

### http.put(pattern: $str; handler: funcdecl): void

Registers `handler` for all `PUT` requests matching `pattern`. Commonly used for full-resource replacement endpoints.

```text
http.put("/items/:id"; &handler);
```

### http.delete(pattern: $str; handler: funcdecl): void

Registers `handler` for all `DELETE` requests matching `pattern`. The request body is typically empty.

```text
http.delete("/items/:id"; &handler);
```

### http.patch(pattern: $str; handler: funcdecl): void

Registers `handler` for all `PATCH` requests matching `pattern`. Use for partial-update endpoints.

```text
http.patch("/items/:id"; &handler);
```

## Server Start Functions

### http.serve(port: u64): i64

Starts an HTTP server on the given port, blocking the current process until the server is shut down. Route handlers registered via `http.get`, `http.post`, etc. are dispatched automatically.

```toke
m=example;
i=http:std.http;
f=main():i64{
  http.getstatic("/"; "hello");
  http.serve(8080);
  <0;
};
```

### http.serveworkers(port: u64; workers: u64): i64

Starts an HTTP server using a pre-fork worker pool. Passing `workers=1` runs in a single process with no fork at all; values greater than one fork that many worker processes before accepting connections, which improves throughput on multi-core systems. Passing `workers=0` auto-detects the CPU count, overridable with the `TK_HTTP_WORKERS` environment variable.

`http.serve(port)` is `http.serveworkers(port;2)`, not the single-process path — so a measurement taken against `http.serve` is a measurement of two processes, and `http.serveworkers(port;1)` is the one to use when you need a single PID to watch.

```toke
i=http:std.http;
f=main():void{
  http.serveworkers(8080;4);
};
```

### http.servetls(port: u64; cert_path: $str; key_path: $str): i64

Starts an HTTPS server with TLS termination, loading the certificate and private key from the given PEM file paths. The server blocks until shut down and then returns `0`; if it cannot start — the certificate or key cannot be loaded, the port cannot be bound — it returns `-1` at once and the process exits 1 (see [Starting, failing and the port](#starting-failing-and-the-port)). It runs a pre-fork worker pool sized to the CPU count, overridable with the `TK_HTTP_WORKERS` environment variable; use `http.servetlsworkers` to choose the count in the program.

```toke
i=http:std.http;
f=main():void{
  http.servetls(443;"/etc/certs/server.crt";"/etc/certs/server.key");
};
```

**Key exchange (story 122.15).** Every `std.http` TLS context -- the `servetls` family's, each SNI host's, and the client's for `https://` URLs -- sets its own key-exchange group list, the same one `std.tls` uses: the post-quantum hybrid `X25519MLKEM768` first, then `X25519`, `P-256`, `X448`, `P-384`. With an OpenSSL that does not know the hybrid (before 3.5) the context falls back to the classical groups instead of failing. Because the list is set by toke, a system `openssl.cnf` `Groups` line no longer decides it (before 122.15 these contexts set no list and inherited the library default). To see what was negotiated, set `TK_TLS_TRACE_GROUP=1` in the environment: each TLS context then writes `tk-tls-groups: hybrid` (or `classical`) to stderr when it is built, and each completed handshake writes `tk-tls-group: server X25519MLKEM768` (or `client ...`, with the group's name). It is a diagnostic switch, off by default; there is no function that returns the group of an `std.http` connection (`std.tls` has `tls.group`). TLS 1.2 is still accepted, and a TLS 1.2 handshake has no post-quantum key exchange; whether to require TLS 1.3 is an open owner decision.

### http.servetlsworkers(port: u64; cert_path: $str; key_path: $str; workers: u64): i64

`http.servetls` with the worker count chosen by the caller, as `http.serveworkers` is for plain HTTP. `workers=1` runs in a single process with no fork; values greater than one fork that many workers; `workers=0` auto-detects the CPU count, overridable with `TK_HTTP_WORKERS` — which is what `http.servetls` always does. Returns `-1` if the certificate or key cannot be loaded or the port cannot be bound; otherwise the server blocks until shut down. (Story 127.192: before this, a program had no way to set the TLS worker count.)

```toke
i=http:std.http;
f=main():void{
  http.servetlsworkers(443;"/etc/certs/server.crt";"/etc/certs/server.key";4);
};
```

### http.serveon(host: $str; port: u64): i64

`http.serve` bound to one address. `host` is an IPv4 literal such as `"127.0.0.1"`, or `"localhost"` (which means `127.0.0.1`), or `""`, `"*"` or `"0.0.0.0"` for all interfaces. Any other host — a name to resolve, an IPv6 address — is refused: the call returns `-1` and the start fails as described under [Starting, failing and the port](#starting-failing-and-the-port), rather than falling back to all interfaces. (Story 138.1.)

```toke
i=http:std.http;
f=main():i64{
  http.getstatic("/health"; "ok");
  http.serveon("127.0.0.1";8080);
  <0
};
```

### http.serveworkerson(host: $str; port: u64; workers: u64): i64

`http.serveworkers` bound to one address; `host` as for `http.serveon`.

### http.servetlson(host: $str; port: u64; cert_path: $str; key_path: $str): i64

`http.servetls` bound to one address; `host` as for `http.serveon`.

### http.servetlsworkerson(host: $str; port: u64; cert_path: $str; key_path: $str; workers: u64): i64

`http.servetlsworkers` bound to one address; `host` as for `http.serveon`.

## Starting, failing and the port

**Which address.** The port-only forms — `http.serve`, `http.serveworkers`, `http.servetls`, `http.servetlsworkers` — listen on **all interfaces**. That is a recorded decision (story 138.1), not an oversight: it is what every deployed toke server already relies on, and changing it would take those servers off the network with no compile error and no runtime message. A program that must be reachable only from its own machine says so at the call, with the `...on` form and `"127.0.0.1"`. `http.listen("127.0.0.1:8080"; handler)` binds the host in its address (before 138.1 the host was dropped and the server listened on every interface).

**Port 0.** Port `0` asks the operating system for any free port. Once the server is listening it prints one line to stderr naming the port it got:

```text
DIAGNOSTIC: kind=server_listening port=54321
```

This is how a supervisor or a test learns the port without picking one first — picking a free port and then starting the server leaves a window in which another process can take it. A port outside `0`–`65535` is refused. (Story 135.24: before this, any port `<= 0` or `> 65535` silently became `8080`, or `8443` for TLS.)

**A server that does not start fails the process.** Every server call returns `-1` if the server could not start — a host or port it refuses, a certificate or key it cannot load, a port it cannot bind, a worker it cannot fork — and returns only then; a server that started returns `0` when it is shut down with `SIGTERM` or `SIGINT`. A start failure also prints one structured line to stderr,

```text
DIAGNOSTIC: kind=server_start_failed fn="http.serveworkers" port=8080 stage="bind/fork"
```

where `stage` is `host`, `port`, `certificate/key` or `bind/fork`, and it makes the process **exit non-zero** however `main` ends — with `main`'s own status if that is non-zero, otherwise `1`, so even a program that ignores the `-1` and returns `0` exits `1`. To a supervisor, a server that never started must not look like a clean shutdown. (Stories 135.22 for the TLS forms, 135.24 for the plain ones; the exit rule is shared with failed `std.test` assertions, see `docs/stdlib/test.md`; 135.29 keeps `main`'s non-zero status.)

**`SIGHUP` reopens the logs; it does not stop the server** (story 121.53). A
running server treats `SIGHUP` — what `logrotate` sends after renaming a log —
as "reopen the access and error logs at their paths", in every mode: the
single-process server and each worker reopen them at their next connection;
the plain pre-fork supervisor reopens them and replaces its workers (its
reload, story 59.4.8); the TLS supervisor reopens them and passes the signal to
its workers. A log opened with `log.openaccess` in a directory the server can
write follows a rename on its own, on the next line written; `SIGHUP` is what
moves one opened where no lock file could be made beside it. Until then
`SIGHUP` killed a single-process server, and killed a TLS supervisor while
leaving its workers listening on the port.

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

The https server's records are written by OpenSSL, through `write(2)`, which
the funnel cannot reach, and on macOS `SO_NOSIGPIPE` is refused (`EINVAL`) on a
socket whose client has already gone — a client that sends its ClientHello
and resets the connection before `accept()` returns. Since 127.394 the server attaches each
accepted socket to OpenSSL with the same `MSG_NOSIGNAL` write BIO the https
client uses (below), so that client costs a failed handshake, not the server.
`test/conform/C1550_https_server_peer_close_no_sigpipe.sh` covers it.

### The client side

The same holds for `std.http`'s client: a server that closes while a request
is still being sent gives the caller `$err`, not a dead process. Plain-HTTP
requests use the per-socket and per-send pair above (127.341). An `https://`
request is written by OpenSSL, whose own socket BIO writes with `write(2)`;
the client therefore attaches its socket to OpenSSL with a write BIO of its
own that sends each record with `MSG_NOSIGNAL` (127.346), so the guard holds on
Linux as well as on macOS, again without touching the program's `SIGPIPE`
disposition. `test/conform/C1260_http_client_peer_close_no_sigpipe.sh` and
`C1301_https_client_peer_close_no_sigpipe.sh` cover the two.

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

Since 127.394 the server's TLS records go through the `MSG_NOSIGNAL` write BIO
described above on every platform, so this disposition is no longer what
protects them. It stays until removing it has been verified on Linux (owner
decision 3.6, `docs/owner-decisions-2026-11-12.md`).

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

## How a request is framed

Every `http.serve*` and `http.servetls*` server frames an HTTP/1.1 request the
same way, and answers a request it cannot frame with a status and a closed
connection rather than a guess (story 121.17):

| The request carries | Answer |
|---|---|
| a header name followed by whitespace before the colon, a header line with no colon, or a folded (indented) continuation line | `400` |
| a `Content-Length` that is not plain digits (`5x`, `+5`, `5, 5`), or two `Content-Length` headers that differ | `400` |
| both `Transfer-Encoding` and `Content-Length` | `400` |
| `chunked` anywhere but last in `Transfer-Encoding`, or twice | `400` |
| any transfer coding other than `chunked` (`gzip, chunked`) | `501` |
| a chunk whose data is not followed by CRLF, or a chunk-size line with no hex digits | `400` |
| a `Content-Length`, or a chunked body, larger than the body limit | `413` |

A chunked body is decoded before the handler runs, on the TLS servers as well
as the plain ones, so the handler's body is the decoded bytes. Requests that
arrive back to back on one connection (pipelining) are answered in order.

**Every request has a deadline.** The whole header block must arrive within
the server timeout (10 seconds by default) of the request starting, and the
body within the timeout plus one second per KiB it announced — a chunked body
is allowed as long as the body limit would take at that rate. A client that
trickles bytes to hold a worker open is answered `408` when its deadline
passes; the timeout used to restart with every byte received.

**HTTP/2 has the same clock** (story 121.44). HTTP/2 is offered on the TLS
servers, through ALPN (`h2` first, then `http/1.1`), so a client that offers
both gets HTTP/2. Between 2026-05-16 and story 121.80 the servers offered only
`http/1.1`, and no client could reach HTTP/2 at all. The connection preface and the client's first
`SETTINGS` frame must arrive within the server timeout of the connection
starting; every frame must be whole within the timeout of its first byte; and
a request's header block (`HEADERS` up to `END_HEADERS`) must be complete
within the timeout of its first frame, and its body on the HTTP/1.1 clock
above. A connection that misses one of these is closed. A header block larger
than the 64 KiB header-list size the server advertises ends the connection
with `GOAWAY` (`ENHANCE_YOUR_CALM`). Waiting between requests is not a
request, but it is not free either: a connection with no request in progress
for 2 seconds — the same as an idle HTTP/1.1 keep-alive connection — is
closed with `GOAWAY` (`NO_ERROR`), and `PING` frames do not count as activity
(story 121.46 — the server used to ping an idle connection and take the
answer as activity, so any client that answered held a worker for as long as
it stayed connected). A worker serves one connection at a time, so an idle
connection costs a worker whichever protocol it speaks; until story 121.80
the HTTP/2 limit was 30 seconds, long enough for one browser's idle
connection to keep a worker from every other client.

**An HTTP/2 request is the same request** (story 121.46). A header block that
continues into `CONTINUATION` frames is dispatched when it ends; the request
body arrives in `req.body`, as on HTTP/1.1, with the same body limit (`413`
past it); and the response carries the handler's own headers (sent with
lower-case names; `Connection`, `Keep-Alive`, `Transfer-Encoding`, `Upgrade`
and `TE` are dropped, as RFC 9113 §8.2.2 requires, and `Content-Length` is
always the length of the body sent), the security headers and
`Strict-Transport-Security`, and the CORS headers when an allowed origin
matched. Until then a request whose headers used `CONTINUATION` was never
answered, `req.body` was always empty, and a response carried only its
status, length and content type.

**HTTP/2 flow control and header compression** (story 121.50). A response
body is sent within the client's flow-control windows (RFC 9113 §6.9): what
the stream's or the connection's window does not take waits and is sent as
the client's `WINDOW_UPDATE` frames open it, while other streams on the
connection are served. A client that never opens its window is disconnected
after `timeout_secs` from the last byte sent. A `WINDOW_UPDATE` that would
take a window past 2^31−1 ends the connection with `FLOW_CONTROL_ERROR`. When
the client changes `SETTINGS_HEADER_TABLE_SIZE`, the next response header
block opens with the dynamic table size update RFC 7541 §4.2 requires, and
the server's table never grows past 4096 bytes. Until then a body over 64 KiB
overran a client on the default window, and the table size changed silently.

**HTTP/2 responses are compressed like HTTP/1.1 ones** (story 121.87). A
`2xx` response body of at least 256 bytes is sent gzip-encoded, with
`content-encoding: gzip` and `vary: Accept-Encoding`, when the request's
`accept-encoding` names `gzip` and the handler set no `Content-Encoding` of
its own — per stream, by the rule the HTTP/1.1 servers use. A `HEAD` carries
the GET's `content-length` and `content-encoding` (story 121.100). Until then HTTP/2 never compressed, so a server sent the
same stylesheet gzipped over HTTP/1.1 and whole over HTTP/2.

**Already-compressed types are sent as they are** (story 121.89). On all
three transports gzip skips a body whose `Content-Type` is `image/*` (except
`image/svg+xml`, which is text), `video/*`, `audio/*`, `font/*`,
`application/octet-stream`, `application/wasm`, `application/zip` or
`application/gzip`, ignoring case and parameters; HTML, CSS, JavaScript,
JSON and SVG still compress. Until then a PNG, a font or a wasm module served
to a gzip-accepting client was gzipped for no gain. std.router's gzip
middleware uses the same list.

**`Accept-Encoding` is read with its weights** (story 121.94). On all three
transports a client is sent gzip only when its `Accept-Encoding` admits it
under RFC 9110 §12.5.3: `gzip` (or `x-gzip`) listed with a weight above zero,
or, when gzip is not listed, `*` with a weight above zero. So `gzip;q=0`,
`*;q=0` and `gzip;q=0, *` get the body as it is, and a weight that is not a
valid qvalue counts as zero. Until then the server looked for the letters
`gzip` anywhere in the header, so a client that refused gzip with `gzip;q=0`
was sent it. std.router's gzip middleware reads the header the same way.

**The handler's security headers win** (story 121.50). Every response
carries `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY` and
`Referrer-Policy: strict-origin-when-cross-origin`, and over TLS
`Strict-Transport-Security: max-age=63072000; includeSubDomains; preload` —
except a field the handler set itself (the name compared without regard to
case), which is sent once, with the handler's value. That is so on HTTP/1.1
and HTTP/2 alike. Until then HTTP/1.1 sent the handler's value and then the
default, so `X-Frame-Options: SAMEORIGIN` went out beside `DENY` — and a
browser that sees two `X-Frame-Options` values applies neither.

**No cleartext upgrade to HTTP/2.** A request carrying `Upgrade: h2c` is
answered over HTTP/1.1 like any other; the server ignores the header, as RFC
9110 §7.8 allows. The upgrade used to answer `101 Switching Protocols` and
then close the connection without serving anything, and RFC 9113 §3.1
deprecates it.

### http.getstatic(path: $str; html: $str): i64

Registers a static HTML response for the given path. When the server receives a request matching `path`, it returns the pre-rendered `html` string as a `200 OK` response with `Content-Type: text/html`. Use this for pages whose content is known at startup.

```toke
i=http:std.http;
f=main():void{
  http.getstatic("/"; "<html><body><h1>Hello</h1></body></html>");
  http.getstatic("/about"; "<html><body><p>About page</p></body></html>");
};
```

### http.setnotfound(body: $str): i64

Sets the body served, with status `404` and `Content-Type: text/html; charset=utf-8`,
for any request whose path matches no registered route — in every serving mode
(plain, TLS, HTTP/2, single-process and workers). The string is copied, so the
caller may free or reuse it afterwards. Returns `0`, or `-1` if the copy could
not be allocated (the previous body stays in force). Without a call, an
unrouted path gets the plain-text body `Not Found`.

```toke
i=http:std.http;
f=main():void{
  http.getstatic("/"; "<h1>Home</h1>");
  http.setnotfound("<h1>404</h1><p>No such page.</p>");
  http.serve(8080);
};
```

A path that a route *does* match keeps whatever that route answers, and a
path whose route exists for another method is `405`. A `*` route — which is
what an `http.servedir` or `http.servepages` mount sits on — is a fallback, not
a route for the path, so it never makes a path `405`: `PUT /anything` on a
server with a mount is `404` unless a route naming that path exists for
another method, and a non-GET to a file under a mount is `404` too (story
121.87; it used to be `405` for every non-GET to every path once any mount
existed). A miss under
`http.servedir` or `http.servepages` — a missing file, or any path no mount
covers, since a mount answers every path no other route takes — is a
`404` with the set body (story 121.81; it used to be the plain `Not Found`
whatever `http.setnotfound` said). Until story 121.62
the set body was served only on one internal fallback that an unrouted path
never reached, so it had no visible effect; it also kept the caller's pointer
instead of a copy.

### How a request path is matched

Routes match the request path **as the client sent it**, up to the first `?`
or `#`, with no percent-decoding. So `http.getstatic("/a b"; …)` is not
reached by `/a%20b`, and a route registered as `/<script>` answers a client
that sends the raw bytes `/<script>` but not `/%3Cscript%3E` (which is a 404).
This is the same rule as `http.param` below. It is deliberate for routing: RFC 3986 §6.2.2.2
makes a percent-encoding equivalent to its character only for unreserved
characters, and `<`, `>` and space are not unreserved, so `/%3Cscript%3E` and
`/<script>` are different paths. Register routes in the form clients send.

`http.servedir` is the exception, for the file name only: the mount is chosen
on the raw path like any route, and the file is then looked up by the
**percent-decoded** path, so `/static/a%20b.txt` serves `a b.txt` (story
121.81; until then such a file could not be served at all). What must not
reach the filesystem is refused: a `%` not followed by two hex digits, `%00`
and `%2F` (an escaped slash) are `400`, and a `..` in the decoded path —
`%2e%2e` as well as `..` — is `403`. `+` is a plain character in a path.

### http.servedir(prefix: $str; dir: $str): i64

Mounts the local directory `dir` under the URL path prefix `prefix`. Any
request to a path beginning with `prefix` is resolved against `dir`. Useful
for serving static assets such as CSS, images, or JavaScript.

The request path is appended to `dir` **whole, including the prefix** — so
`http.servedir("/static";"./public")` serves `/static/style.css` from
`./public/static/style.css`, not from `./public/style.css`. Lay the directory
out to match.

**Files are served as bytes.** A file holding `0x00` bytes — an image, a
font, a `.wasm` module — is served whole, with a `Content-Length` counting
every byte, over HTTP/1.1, TLS and HTTP/2 (since story 121.100 cleartext
HTTP/1.1 too, where it was sent in chunks, so a `HEAD` gives the file's
size), and gzip-encoded
whole when the client accepts it (story 121.86). Until then every response
writer sized the body as a C string, so a binary file was cut at its first
`0x00`: a PNG arrived as its 8-byte signature and nothing else. A handler's
body is a `str`, which cannot hold a `0x00`, so this only ever affected
`http.servedir`.

**Precompressed files are served when the client accepts them** (story
121.99). When `style.css.br`, `style.css.gz` or `style.css.zst` sits beside
`style.css` and the request's `Accept-Encoding` admits that coding (RFC 9110
§12.5.3: weights and `*` count, `gzip;q=0` refuses), the variant is sent in
place of the file — `br` before `gzip` before `zstd` — with the file's
`Content-Type`, `Content-Encoding` naming the coding, and `Vary:
Accept-Encoding`; it is not compressed again. A directory's `index.html` is
served the same way. A file that has any variant says `Vary: Accept-Encoding`
even when it is sent as it is, so a cache keeps the encodings apart. The
variant is resolved and checked like the file itself: one that is a symlink
out of `dir` is not followed, and the file is sent instead. `http.servedir`
sends no `ETag`. Until then the variants were never served: the file was read
and gzipped per request, and a client that accepts only `br` got it whole.

**Capabilities.** The mount gives the server the authority to read any file
under `dir` on every request, so `http.servedir` — and `http.servepages`,
which reads its pages and templates on every request — takes `fs.read` when it
is called, as well as the `net` the server takes to listen (story 121.42).
`http.downloadfile` takes `fs.write` as well as `net`. Under enforcement a
missing grant is `CAP001` at the call.

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

### http.param(req: $req; name: $str): $str!$httprouteerr

Extracts a named URL parameter captured by the route pattern (e.g., the `id` from `"/users/:id"`). Returns `$httprouteerr.$notfound` if no parameter with that name was captured for this request. A parameter that is present and empty is `$ok ""`. (Until story 135.28 a missing parameter was also `$ok ""`.)

The value is the path segment exactly as the client sent it: it is **not** percent-decoded, so `/users/a%20b` gives `a%20b` and `%2e%2e` stays `%2e%2e`. If you decode it, treat the result as untrusted before it reaches a file path or a query — a decoded value can contain `../` or a NUL byte that the raw one could not.

Today the `$err` arm is taken but its payload cannot be matched on: `mt e {$notfound:m ...}` inside it is E9005, because the compiler does not yet bind a payload for this call. Use the arm itself as "missing". `http.reqparam` is the plain-`str` form: `""` when missing.

```text
let id = http.param(req; "id");
-- id = ok("42") when path matched "/users/:id"
```

### http.header(req: $req; name: $str): $str!$httprouteerr

Extracts a request header value by name using case-insensitive lookup. Returns `$httprouteerr.$notfound` if the header is not present in the request; a header present with an empty value is `$ok ""` (story 135.28). As for `http.param`, the `$err` payload cannot yet be matched on. `http.reqheader` is the plain-`str` form: `""` when missing.

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

Creates a 200 OK response with the given body and `Content-Type: text/html; charset=utf-8`. For another content type build the `$res` as a literal.

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

## Testing a Handler

A route handler is an ordinary function, `(req:$req):$res`. A test calls it directly: `http.mockreq` builds the request the server would hand it, and `http.resstatus`, `http.resbody` and `http.resheader` read what it returned. No server starts and no socket opens (story 138.16).

`http.mockreq` builds the same request the server's dispatchers build, so every field a handler reads (`req.body`, `req.headers`, ...) and every accessor it calls (`http.reqmethod`, `http.reqpath`, `http.reqbody`, `http.reqparam`, `http.reqheader`, `http.param`, `http.header`) reads what it would read under the server. Two things are different:

- **Route params are given, not matched.** The server binds `:id` from the route pattern; a test passes the bindings it wants in `params`.
- **The response is the handler's, not the wire's.** The `res*` functions read the response the handler returned. The server adds `Content-Length`, `Connection` and any CORS header when it sends, and those are not in it.

A handler declared `(req:i64):i64` cannot take the `$req` from `http.mockreq` (E4031); declare it `(req:$req):$res`. The route registration and the accessors accept both forms.

### http.mockreq(method: $str; path: $str; headers: @(@($str)); body: $str; params: @(@($str))): $req

Builds a request. `headers` and `params` are lists of `@(name; value)` pairs; pass `@()` for none. Header lookups are case-insensitive, as under the server. An element that is not a two-element `@(name; value)` stops the program with exit status 1 and a message naming the argument, because a test that carried on would be testing a different request from the one it wrote. The request keeps the arrays it was given, so `req.headers` is the very list the test passed. The request is never freed.

### http.resstatus(res: $res): u64

The status the handler set.

### http.resbody(res: $res): $str

The body the handler set.

### http.resheader(res: $res; name: $str): $str

The value of the handler's header `name`, compared case-insensitively; `""` when the handler set no such header. The response constructors set `Content-Type`.

```toke
m=handlertest;
i=http:std.http;
i=io:std.io;
f=greet(req:$req):$res{
  let who=http.reqparam(req; "name");
  if(who==""){ <http.res.bad("no name") };
  <http.resnew(200; who)
};
f=main():i64{
  let ok=greet(http.mockreq("GET"; "/hello/ada"; @(); ""; @(@("name"; "ada"))));
  let bad=greet(http.mockreq("GET"; "/hello/"; @(); ""; @()));
  if(http.resstatus(ok)!=200){ <1 };
  if(http.resbody(ok)!="ada"){ <2 };
  if(http.resstatus(bad)!=400){ <3 };
  io.println(http.resheader(ok; "content-type"));
  <0
};
```

## HTTP Client Functions

### http.client(baseurl: $str): $httpclient

Creates an HTTP client configured to send all requests relative to `baseurl`. The returned handle maintains a connection pool and a default per-read timeout (`timeoutms`, 30 s). Pass it to the client calls (`http.clientget` and the rest), to `http.stream` and `http.downloadfile`, and to the options that return a changed copy: `http.withproxy` and the [request controls](#request-controls).

```toke
i=http:std.http;
f=main():void{
  let c=http.client("https://api.example.com");
};
```

### http.withproxy(client: $httpclient; proxyurl: $str): $httpclient

Returns a copy of `client` that sends its requests through the HTTP proxy at `proxyurl` (`http://host:port`); `client` itself is unchanged. The client calls (`http.clientget` and the rest) use it. A plain `http://` request goes to the proxy with the absolute URL in its request line. An `https://` request first asks the proxy for a tunnel (`CONNECT host:port`) and then negotiates TLS with the server through it. The server's certificate is verified exactly as without a proxy. A proxy that refuses the tunnel is `$err`. Before story 135.26 the client started TLS with the proxy itself, so every https request through a proxy failed.

### Request controls

The next three functions are the client's request controls (stories 138.24 and 138.25; spec amendment [138.24](/docs/decisions/138.24-http-client-controls/)). They exist for a program that fetches URLs it does not control. Each works like `http.withproxy`: it returns a copy of the client with one setting changed and leaves the original unchanged, so the controls can be combined in any order. A client from `http.client` has none of them, and every call behaves as it always has.

They apply to `http.clientget`, `http.clientpost`, `http.clientput` and `http.clientdelete`. `http.stream` and `http.downloadfile` read on separate code paths, so they refuse a client that carries any of the controls: `http.stream` answers `$err`, and `http.downloadfile` returns `-1`. Silently ignoring a control could fetch something the caller meant to refuse. Each failure below is an `$httperr` with `code` 0, a transport failure.

```toke
m=fetchdemo;
i=http:std.http;
i=net:std.net;
i=io:std.io;
f=fetch(name:$str;addr:$str):$str{
  let c=http.client("https://\(name)");
  let c2=http.withmaxbody(http.withdeadline(http.withconnectaddr(c;addr);5000);1048576);
  <mt http.clientget(c2;"/data.json") {$ok:r "status \(r.status), \(r.body.len) bytes"; $err:e e.msg}
};
f=main():i64{
  let addrs=mt net.resolve("example.com") {$ok:a a; $err:e @("")};
  io.println(fetch("example.com";addrs.get(0)));
  <0
};
```

A real caller checks every address `net.resolve` returns against its own rules (private, loopback and link-local ranges, cloud metadata addresses) before it connects to one.

### http.withdeadline(client: $httpclient; ms: u64): $httpclient

Returns a copy of `client` whose requests must each finish within `ms` milliseconds of the call. The deadline covers the whole request:

- connecting (and the proxy `CONNECT`, when there is a proxy);
- the TLS handshake;
- sending the request;
- reading the response headers and body.

A request that runs past it is `$err`. Its `msg` begins `request deadline exceeded (<ms> ms)`, then names the stage the request had reached, for example `request deadline exceeded (5000 ms) after 3 of 8 body bytes (Content-Length)`, `... during the TLS handshake` or `... before the header block ended (12 bytes)`. The deadline does not restart when data arrives, so it cuts off a server that sends one byte at a time inside `timeoutms`. `timeoutms` still applies to each wait within the deadline. A deadline of `0` has already passed, so every request fails at once. No value turns the deadline off: to drop it, use the client it was added to.

A lookup of the URL's host name cannot be interrupted, because `getaddrinfo` blocks. The deadline is checked before the lookup starts and again when it returns. With `http.withconnectaddr` there is no lookup.

### http.withmaxbody(client: $httpclient; bytes: u64): $httpclient

Returns a copy of `client` whose requests answer `$err` for a response body longer than `bytes`. The read stops there: the connection is closed with the rest unread, and a cut-short body is never answered `$ok`. The `msg` begins `response body exceeds the cap (<bytes> bytes)`. When the check happens depends on how the body is framed:

- **`Content-Length`:** a length over the cap is refused before the body is read (`...: Content-Length is 67108864`).
- **Chunked:** the body is refused on the size line of the chunk that would take it past the cap, before that chunk's data is read. A size line, chunk extension or trailer line longer than the room the cap has left plus 8 KiB is refused too.
- **Neither header (the body ends when the connection closes):** the body is refused as soon as it passes the cap.

A body exactly `bytes` long is `$ok`. A cap of `0` admits only an empty body. The cap counts the body as the response delivers it (`r.body`, decoded from chunks). A `HEAD`, `204` or `304` response has no body, so it never exceeds the cap. The response header block is not capped.

### http.withconnectaddr(client: $httpclient; addr: $str): $httpclient

Returns a copy of `client` that connects to `addr` instead of looking up the URL's host. This is the equivalent of `curl --resolve`. `addr` is an IPv4 or IPv6 address literal (`"203.0.113.7"`, `"2001:db8::7"`), written without brackets, port or zone. It is connected to without any lookup, on the URL's port. Everything else still uses the URL's host:

- the `Host` header;
- the TLS server name (SNI);
- the name the server's certificate must carry (verified as for every `https://` request).

A program can therefore resolve a name with [`net.resolve`](/docs/stdlib/net/), check every address it gets, and connect to the address it checked without the name being resolved a second time to something else (DNS rebinding).

`addr` is checked when a request is made, since this function has no error channel:

- an `addr` that is not an address literal, such as `"localhost"`, is `$err` (`withconnectaddr: not an IPv4 or IPv6 address literal`) and nothing is sent;
- a client that has both `withconnectaddr` and `withproxy` is `$err` (`withconnectaddr cannot be combined with withproxy`), because the proxy makes its own connection to the URL's host.
- under a scoped `net` grant (138.19), both the URL's host and `addr` must be admitted by a `connect:` scope, because `addr` is the address actually dialled. A grant for the host alone does not reach an arbitrary address (CAP002 names whichever is missing).

### Client calls and route registration are different names

`http.get`, `http.post`, `http.put`, `http.delete` and `http.patch` **register routes** — `http.get("/health"; &handler)` — and send nothing. The client calls have names of their own (story 138.3): before them, `http.get(client;"/health")` compiled and registered a GET route whose path was the client handle (nothing was sent; loke saw the call "succeed" from the 257th time on, once the route table was full), a client DELETE could not be reached at all, and `http.get(url)` returned `""` for a server that was not there. `http.get(url)` with one argument still compiles today to an untyped GET; use `http.geturl`, which has an error channel.

A client call's `$err` is a **transport** failure: no connection, a send or receive that failed, a URL that does not parse or is refused (below), HTTPS without TLS in the build. An HTTP status is not an error — `404` is a response, and `r.status` is `404`. The error is an `$httperr` (`msg`, `code`).

**A URL that would rewrite the request is refused before anything is sent** (story 138.24). The URL's host, port and path go into the request line and the `Host` header as they are, so a line break in any of them would let the URL add headers, or a second `Host`, of its own. Every client call — `http.geturl`, `http.postheaders`, `http.getheaders`, the `http.client…` calls, `http.stream` and `http.downloadfile` — therefore requires:

- a host that is a DNS name or an IPv4 address: ASCII letters, digits, `-`, `.` and `_`. A bracketed IPv6 literal is not parsed; connect to an IPv6 address with [`http.withconnectaddr`](#httpwithconnectaddrclient-httpclient-addr-str-httpclient);
- a port and a path with no control character (U+0000–U+001F, U+007F) and no space. Percent-encode a space as `%20`. Bytes above `0x7f` are sent as they are.

A refused URL is `$err` (`code` 0) with the message `URL refused: the host must be a DNS name or an IPv4 address, and the port and path may not hold a space or a control character`. `http.downloadfile` returns `-1`, and a refused proxy URL is `failed to parse proxy URL`. Before this, a URL holding a line break was sent with the smuggled lines in it, and one holding a space or tab drew a `400` from the server.

A response that ends before it is complete is also a transport failure (`code` 0): the connection closes, or a read times out, before the blank line that ends the headers, before all the bytes its `Content-Length` promises, or before the last (zero-size) chunk of a `Transfer-Encoding: chunked` body. `msg` says which, e.g. `incomplete response: connection closed after 3 of 8 body bytes (Content-Length)`. Until story 138.23 each of these answered `$ok` with whatever had arrived. A body with neither header ends when the server closes the connection; a `204`, a `304` and a `1xx` response have no body.

**A body with neither `Content-Length` nor chunked encoding ends only at a close** (story 138.31, RFC 9112 §6.3). The server closing the connection ends it — on `http://` the read returns end-of-stream; on `https://` the server sends a TLS `close_notify` alert, or closes the TCP connection without one (see below). A read that **times out** (the client's per-read timeout, 30 s by default), a connection **reset**, any other socket error, or a TLS error before that close is a transport failure (`code` 0): the body was cut. `msg` names the cause and how much arrived, e.g. `incomplete response: receive timed out after 3 body bytes of a body with no Content-Length (it ends only at connection close)` or `... connection reset by peer after ...`. Until then each of these answered `$ok` with the bytes that had arrived. The same causes are named in the incomplete-header, `Content-Length` and chunked messages above (`receive timed out`, `connection reset by peer`, `connection closed (TLS, without close_notify)`, `TLS error (...)`, `receive failed (...)`), which used to say only `receive timed out or failed`.

**HTTPS: a close without `close_notify` counts as the end of the body.** Many servers close the TCP connection after the last byte without sending the TLS closure alert. OpenSSL 3.0 and later (Homebrew ships 4.0.x) report that as `SSL_ERROR_SSL` with reason `SSL_R_UNEXPECTED_EOF_WHILE_READING` (older releases: `SSL_ERROR_SYSCALL` with `errno` 0); the client reads either as a close, so such a body is answered `$ok`. It does not set `SSL_OP_IGNORE_UNEXPECTED_EOF`, so the distinction stays visible: a short `Content-Length` or chunked body cut that way still fails, and its `msg` says `connection closed (TLS, without close_notify)`. This is the common practice (Python's `ssl` module does the same by default, `suppress_ragged_eofs=True`), chosen so ordinary `https://` bodies that worked before keep working. The stricter alternative — RFC 9112 §9.8, under which a close-delimited response over TLS is complete only after a valid closure alert, because a truncated one is otherwise indistinguishable from a whole one — would answer such bodies with an error; it is open as an owner decision, and is one line in `src/stdlib/http.c` (`CLIENT_RECV_IS_CLOSE`). A body with a `Content-Length` or chunked framing is checked against its framing either way, so the truncation this permits is limited to close-delimited bodies.

The client sends `Host: host:port` when the URL names a port other than the scheme's default (`80` for `http://`, `443` for `https://`), and the bare host otherwise (RFC 9110 §7.2; story 138.28 — it used to send the bare host on every port).

### http.clientget(client: $httpclient; path: $str): $httpresp!$httperr

Sends `GET path` relative to the client's base URL.

```toke
i=http:std.http;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let c=http.client("http://127.0.0.1:8080");
  let up=mt http.clientget(c;"/health") {$ok:r r.status=200; $err:e false};
  io.println("up=\(up)");
  <0
};
```

### http.clientpost(client: $httpclient; path: $str; body: @(byte); content_type: $str): $httpresp!$httperr

Sends `POST path` with the given body and `Content-Type`. Use `str.bytes` for a string body. A `content_type` of `""` sends `application/octet-stream`.

```toke
i=http:std.http;
i=str:std.str;
f=main():i64{
  let c=http.client("http://127.0.0.1:8080");
  let r=mt http.clientpost(c;"/items";str.bytes("{\"name\":\"toke\"}");"application/json") {$ok:r r.status; $err:e 0};
  <0
};
```

### http.clientput(client: $httpclient; path: $str; body: @(byte); content_type: $str): $httpresp!$httperr

`http.clientpost` with `PUT`.

### http.clientdelete(client: $httpclient; path: $str): $httpresp!$httperr

Sends `DELETE path`, with no body.

### http.geturl(url: $str): $str!$httperr

`GET` a full `http://` or `https://` URL and return the body. It has nowhere to put a status, so any status outside `200`–`299` is `$err`, with `code` set to the status; a transport failure is `$err` with `code` 0. Use `http.clientget` when you need the status or headers of an unsuccessful response.

```toke
i=http:std.http;
i=io:std.io;
f=main():i64{
  let body=mt http.geturl("http://127.0.0.1:8080/health") {$ok:b b; $err:e ""};
  io.println(body);
  <0
};
```

### http.postheaders(url: $str; body: $str; headers: @(@($str))): $httpresp!$httperr

`POST` `body` to a full `http://` or `https://` URL with the given headers, each an `@(name; value)` pair. The body is sent as `application/json` unless `headers` carries a `Content-Type`, which then replaces it (story 138.8: it used to be sent in addition, so the server saw two). `Content-Length` and `Host` are set by the client and may not be given; a header name or value containing a carriage return or line feed is refused (`$err`) rather than sent, because it would let the caller write a header of its own choosing. As for the other client calls, `$err` is a transport failure and a status such as `401` is a response.

Over `https://` the server's certificate must verify against the trust store (the system default; `SSL_CERT_FILE` or `SSL_CERT_DIR` override it) and name the host or IP address in the URL. Before story 138.8 no certificate was verified by any `std.http` client call. The key exchange prefers the post-quantum hybrid `X25519MLKEM768`, and `TK_TLS_TRACE_GROUP=1` traces it, as described under [`http.servetls`](#httpservetlsport-u64-cert_path-str-key_path-str-i64).

```toke
i=http:std.http;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let h=@(@("x-api-key";"secret");@("content-type";"application/json"));
  let r=mt http.postheaders("https://127.0.0.1:8443/v1/messages";"{\"n\":1}";h) {
    $ok:r "\(r.status) \(str.frombytes(r.body))";
    $err:e "request failed"
  };
  io.println(r);
  <0
};
```

### http.getheaders(url: $str; headers: @(@($str))): $httpresp!$httperr

`GET` a full `http://` or `https://` URL with the given headers, each an `@(name; value)` pair (story 138.27). It is `http.postheaders` without a body:

- the headers are sent as given, with no `Content-Type` and no `Content-Length` added;
- `Host` and `Content-Length` are set by the client, and giving either is `$err`;
- a header name or value containing a carriage return or line feed is `$err`, and nothing is sent;
- the `url` is held to the same rule: a host, port or path that would rewrite the request is `$err`, and nothing is sent (see [Client calls and route registration are different names](#client-calls-and-route-registration-are-different-names));
- `$err` means a transport failure, and a status such as `404` is a response, in `r.status`;
- over `https://` the server's certificate is verified, as for every client call.

It takes a URL, not a client, so the [request controls](#request-controls) do not apply to it.

```toke
m=getheadersdemo;
i=http:std.http;
i=io:std.io;
f=main():i64{
  let h=@(@("User-Agent";"loke-update/1.0");@("Accept";"application/vnd.github+json"));
  io.println(mt http.getheaders("https://api.github.com/repos/karwalski/toke/releases/latest";h) {
    $ok:r "status \(r.status)";
    $err:e e.msg
  });
  <0
};
```

### http.get(url: $str): $httpresp!$httperr

> **Not exported.** `std.http`'s interface declares `http.get` as route registration (`path; handler`). A one-argument `http.get(url)` does compile today -- `src/llvm.c` rewrites it to `tk_http_get_w` -- but that is a compiler special case: its result is untyped and it has no error channel. Use `http.geturl(url)`.

### http.post(client: $httpclient; path: $str; body: @(byte); content_type: $str): $httpresp!$httperr

> **Not exported.** `http.post` is route registration (`http.post(path;handler)`, or `tk_http_posturl_w` for two non-function arguments), so the four-argument client call is rejected by `tkc --check` with E4026. Use `http.clientpost`.

### http.put(client: $httpclient; path: $str; body: @(byte); content_type: $str): $httpresp!$httperr

> **Not exported.** `http.put` is route registration (`http.put(path;handler)`). Use `http.clientput`.

### http.delete(client: $httpclient; path: $str): $httpresp!$httperr

> **Not exported.** `http.delete` is route registration: `http.delete(c;path)` passes the client handle where a path belongs, type-checks, and sends nothing. Use `http.clientdelete`.

### http.stream(client: $httpclient): $httpstream!$httperr

Opens a streaming HTTP connection for server-sent events or chunked transfer responses. Returns an `$httpstream` handle on success; use `http.streamnext` to read successive chunks. The connection remains open until `open` is `false` or an error is returned. The interface takes the client alone: there is no method or path argument, and the request is a `GET` of the client's base URL -- put the path in the URL you pass to `http.client`. The client is borrowed, not consumed. A failure to connect, send or read the response headers takes the `$err` arm with an `$httperr` whose `msg` says which (`TCP connect failed`, `HTTPS requires TLS (not compiled in)`, ...).

> Until 121.53 the glue (`tk_http_stream_w`) read its one argument as a URL string rather than the client this interface publishes (so the published call was rejected at compile time), and `http.stream` / `http.streamnext` returned a bare 0 with no `$httperr` on failure, so `e.msg` was RT005. `docs/spec/stdlib-signatures.md` still records an unbuilt `(c; req)` form; see the note there.

### http.streamnext(stream: $httpstream): @(byte)!$httperr

Reads the next available chunk of bytes from an open `$httpstream`. Returns an empty byte array when the server closes the connection gracefully; returns `$httperr` on network or protocol error. Call this in a loop until the returned byte array is empty or an error occurs.

```toke
m=streamdemo;
i=http:std.http;
i=io:std.io;
f=main():i64{
  let c=http.client("http://127.0.0.1:8080/events");
  let n=mt http.stream(c) {
    $ok:s mt http.streamnext(s) {$ok:v v.len;$err:e 0};
    $err:e 0
  };
  io.println("first chunk: \(n) bytes");
  <0
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

### $httprouteerr

A sum type representing request-accessor failures, returned by `http.param` and `http.header`.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $badrequest | $str | The request is malformed or the argument is invalid |
| $notfound | $str | The requested parameter, header, or resource was not found |
| $internal | $str | An internal server or client error occurred |
| $timeout | u32 | The operation timed out; value is the timeout in milliseconds |

### $httperr

A struct, not a sum type: the `$err` of the client calls (`http.clientget`, `http.clientpost`, `http.clientput`, `http.clientdelete`, `http.geturl`, `http.postheaders`, `http.getheaders`) and of the stream functions `http.stream` and `http.streamnext`. A request cut off by [`http.withdeadline`](#httpwithdeadlineclient-httpclient-ms-u64-httpclient) or refused by [`http.withmaxbody`](#httpwithmaxbodyclient-httpclient-bytes-u64-httpclient) is one too, with `code` 0 and a `msg` that begins `request deadline exceeded` or `response body exceeds the cap`.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | What failed |
| code | u64 | Numeric error code |
