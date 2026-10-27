---
title: toke Web Server
slug: web-server
section: about
order: 6
---

## toke Web Server

An HTTP/1.1 and HTTP/2 server built into the toke standard library. Import `std.http`, register handlers, call `http.serve` -- no framework, no package manager, no container. Every name below is a call `std.http`'s interface (`stdlib/http.tki`) exports, so it is what a toke program can actually write; behaviour the runtime has but does not expose to toke is listed separately, and says so. The full reference is [std.http](/docs/stdlib/http/).

### What a toke program can call

**Route registration** -- bind a pattern to a handler `f=name(req:$req):$res{...}`, passed by reference:

- `http.get(pattern;&handler)`, `http.post`, `http.put`, `http.delete`, `http.patch`. These names **register routes** and send nothing; the HTTP client calls are `http.clientget`/`clientpost`/`clientput`/`clientdelete` (story 138.3).
- Patterns may carry named segments (`"/users/:id"`), read with `http.param`.
- `http.getstatic(path;body)`, `http.servedir(prefix;dir)` and `http.setnotfound(body)` serve fixed content without a handler.

**Response constructors** -- `http.res.ok(body)` (200, `text/html`), `http.res.json(status;body)` (`application/json`), `http.res.bad(msg)` (400), `http.res.err(msg)` (500). For any other status or content type, build the `$res` record yourself.

**Request accessors** -- `http.param(req;name)` and `http.header(req;name)` return `$str!$httprouteerr` (`$err` when absent; a header lookup is case-insensitive). `req.body`, `req.method` and `req.path` are fields of the request record.

**Starting the server** -- `http.serve(port)`; `http.serveworkers(port;n)` for a pre-fork pool of `n` worker processes; `http.servetls(port;cert;key)` and `http.servetlsworkers(port;cert;key;n)` terminate TLS from PEM files; the `...on` forms (`http.serveon(host;port)` and so on) bind one address. Each blocks until the server stops.

**Testing a handler without a socket** -- `http.mockreq(...)` builds a `$req`, and `http.resstatus`/`resbody`/`resheader` read a `$res` (see [Testing a Handler](/docs/stdlib/http/)).

### What the runtime does for every server

These need no call -- they are how the server behaves:

- **Keep-alive.** An HTTP/1.1 connection idle for 2 s between requests is closed, and one connection serves at most 1000 requests. An HTTP/2 connection (offered over TLS through ALPN) is closed after 30 s with no request in progress.
- **Gzip.** A response is gzip-compressed when the request's `Accept-Encoding` admits it and the content type is worth compressing.
- **Request framing and limits.** Chunked request bodies are decoded by the server. Headers are capped at 8 KiB, bodies at 1 MiB, and each socket read times out after 10 s (see "How a request is framed" in [std.http](/docs/stdlib/http/)).
- **Workers.** In a pre-fork pool the supervisor binds the port once and every worker accepts on the inherited socket; it never sets `SO_REUSEPORT`, so a second server cannot bind a busy port and silently split its traffic (story 127.203). A worker that dies is respawned.
- **Security headers.** Every response carries `X-Content-Type-Options: nosniff`, `X-Frame-Options: DENY` and `Referrer-Policy: strict-origin-when-cross-origin`.
- **No rate limiting.** The per-IP limiter was removed (story 127.60): it counted connections rather than requests, kept a separate table in each worker process, and HTTP/2 clients never reached it. Put rate limiting at an external reverse proxy or WAF.

### In the C runtime, not callable from toke

`src/stdlib/http.c` also implements weak ETags (`http_etag_fnv`, `http_etag_matches`), cookie reading and `Set-Cookie` building (`http_cookie`, `http_set_cookie_header`), `multipart/form-data` and URL-encoded form parsing (`http_multipart_parse`, `http_form_parse`, `http_form_get`), configurable limits (`http_set_limits`) and a graceful drain (`http_shutdown`). **None of them is in `std.http`'s interface**, so a toke program cannot call them today, and the server does not apply ETags or conditional `304` responses on its own. They are listed here so nobody reads an earlier version of this page and goes looking for them.

### Hello World

```toke
m=hello;
i=http:std.http;

f=home(req:$req):$res{
  <http.res.ok("Hello from toke!")
};

f=main():i64{
  http.get("/";&home);
  <http.serve(8080)
};
```

Build and run it. A toke program is denied the network until it is granted it (capabilities, ADR-0010), so pass `--allow-net` when you start it:

```bash
tkc hello.tk --out hello
./hello --allow-net
# In another terminal:
curl http://localhost:8080/
# Hello from toke!
```

Without `--allow-net` the binary exits 1 at once with `CAP001: capability 'net' required but not granted`.

### A small JSON API

Three routes: a named path segment, a JSON echo that checks the request's content type, and a fixed status. Each handler returns a `$res`; `mt` takes the `$ok` and `$err` arms of an accessor apart.

```toke
m=api;
i=http:std.http;
i=str:std.str;

f=greet(req:$req):$res{
  <mt http.param(req;"name") {
    $ok:name  http.res.json(200;str.concat("{\"hello\":\"";str.concat(name;"\"}")));
    $err:e    http.res.bad("missing name")
  }
};

f=echo(req:$req):$res{
  <mt http.header(req;"content-type") {
    $ok:ct  (if(ct=="application/json"){http.res.json(200;req.body)}el{http.res.bad("send JSON")});
    $err:e  http.res.bad("no content-type")
  }
};

f=gone(req:$req):$res{
  <http.res.json(410;"{\"error\":\"gone\"}")
};

f=main():i64{
  http.get("/hello/:name";&greet);
  http.post("/echo";&echo);
  http.delete("/items/:id";&gone);
  <http.serve(8080)
};
```

```bash
tkc api.tk --out api && ./api --allow-net &

curl http://localhost:8080/hello/ada
# {"hello":"ada"}
curl -X POST -H 'Content-Type: application/json' -d '{"a":1}' http://localhost:8080/echo
# {"a":1}
curl -X POST -H 'Content-Type: text/plain' -d 'x' http://localhost:8080/echo
# send JSON          (status 400)
curl -X DELETE http://localhost:8080/items/3
# {"error":"gone"}   (status 410)
curl http://localhost:8080/nope
# Not Found          (status 404: no route matched)
```

`str.concat` builds the JSON by hand here and does not escape `name`; for real output build the body with [std.json](/docs/stdlib/json/). `/hello/` with an empty name matches no route (404), so `greet`'s `$err` arm is the defensive case.

### Production Setup: TLS + Workers

TLS termination and a pre-fork pool of eight workers. Rate limiting and cache policy belong at a reverse proxy in front of it; toke's own controls are deny-by-default capabilities (ADR-0010), auto-escaping and parameterised queries (ADR-0011), and the spatial-safety traps (ADR-0012).

```toke
m=prod;
i=http:std.http;

f=index(req:$req):$res{
  <http.res.ok("running")
};

f=main():i64{
  http.get("/";&index);
  <http.servetlsworkers(443;"/etc/toke/cert.pem";"/etc/toke/key.pem";8)
};
```

Deploy:

```bash
tkc prod.tk --out prod

# Copy to server
scp prod user@server:/opt/toke/prod

# Run with systemd (create /etc/systemd/system/toke-prod.service)
# [Service]
# ExecStart=/opt/toke/prod --allow-net
# Restart=always
# LimitNOFILE=65536

sudo systemctl enable toke-prod
sudo systemctl start toke-prod
```

Binding port 443 needs the privilege your platform requires for a port below 1024. The binary is not fully static: it links the C library, zlib and OpenSSL as shared libraries (`otool -L prod` on macOS, `ldd prod` on Linux), so the server needs those installed. No reverse proxy is needed for TLS.

### Performance

**The cross-language throughput table that stood here is withdrawn (story
132.38).** It compared toke against nginx, Go `net/http`, Node Express and
Rust actix-web on throughput, p95 latency and memory. Nothing backed it:

- The nginx / Go / Node / Rust columns were never measured. The table's own
  note described them as "representative of commonly published benchmarks",
  naming no benchmark, no version, no hardware and no URL.
- No `wrk` harness exists anywhere in this project, so the caption's stated
  method could not be re-run.
- The toke column traced only to a status-ledger line
  (`docs/progress.md`, story 59.2.2), which records ~366 req/s at **10
  concurrent connections** and p95 0.7 ms **on localhost** — not the "16
  workers with keep-alive" the caption claimed. The 61.9 MB memory figure
  appears in no measurement artefact at all, and the 16-worker configuration
  it named was later rolled back after two OOM kills
  (`toke-website/specs/production-config.md`), which set the deployed worker
  count to 8.

This project's rule is that an unsourced claim comes down rather than
acquiring a plausible-looking source, so it is removed rather than rewritten.

There **is** a real, reproducible serving baseline, and it measures something
narrower than the withdrawn table: cold start and TTFB for the site binary
against `caddy file-server`, recorded in `docs/metrics-baseline.md`
(story 134.6) with its harness, raw results and reproduce command under
`toke-ooke/bench/serving/`. It does not compare toke to Go, Node, Rust or
nginx, and no such comparison should be published until one is measured.

What can be stated without a benchmark: it terminates TLS itself, and it
pre-forks workers that accept on one listening socket the supervisor bound
(never `SO_REUSEPORT`, story 127.203). The "~2 MB, no runtime dependencies"
that stood here is withdrawn (119.8): a server binary links OpenSSL, zlib and
the C library as shared libraries, and its size is not a measured figure in
`docs/metrics-baseline.md`.

### How it compares

Capabilities only — these are properties of each stack, not measurements.
The "hello world lines" and third-party binary sizes that stood in this table
are withdrawn with the throughput table above (132.38): they were counted from
unstated sources.

| | toke | Go net/http | Node Express | Rust actix-web |
|---|---|---|---|---|
| Runtime deps | OpenSSL, zlib (shared libraries) | 0 | 30+ (npm) | 0 |
| TLS built-in | Yes | Yes | No (needs proxy) | Yes |
| Worker pool built-in | Yes | Yes (goroutines) | No (needs cluster) | Yes |
| Package manager required | No | No | Yes (npm) | Yes (cargo) |
