---
title: toke Web Server
slug: web-server
section: about
order: 6
---

## toke Web Server

A production-grade HTTP server built into the toke standard library. Single static binary, zero runtime dependencies. Import `std.http` and you have a full-featured web server -- no framework, no package manager, no container.

### Features

**Route registration** -- pattern-matched handlers for all standard HTTP methods:

- `http.get(pattern, handler)` -- `http_GET`
- `http.post(pattern, handler)` -- `http_POST`
- `http.put(pattern, handler)` -- `http_PUT`
- `http.delete(pattern, handler)` -- `http_DELETE`
- `http.patch(pattern, handler)` -- `http_PATCH`

**Response constructors:**

- `http.ok(body)` -- 200 text response (`http_Res_ok`)
- `http.json(status, body)` -- JSON response with status code (`http_Res_json`)
- `http.bad(msg)` -- 400 Bad Request (`http_Res_bad`)
- `http.err(msg)` -- 500 Internal Server Error (`http_Res_err`)

**Request accessors:**

- `http.param(req, name)` -- extract a route parameter (`http_param`)
- `http.header(req, name)` -- extract a request header (`http_header`)

**TLS termination** -- native HTTPS with PEM cert/key loading (`http_serve_tls`, `http_serve_tls_workers`). No reverse proxy required for TLS.

**Pre-fork worker pool** -- multi-process serving via `http_serve_workers`. Specify worker count; `nworkers=1` falls back to single-process mode.

**Keep-alive** -- persistent connections with configurable idle timeout (default 30s, max 1000 requests per connection).

**Gzip compression** -- automatic response compression.

**ETag generation and 304 support** -- `http_etag_fnv` generates weak ETags via FNV-1a hash. `http_etag_matches` checks `If-None-Match` headers for conditional responses.

**Rate limiting** -- per-IP sliding window.

**Cookie support** -- `http_cookie` reads cookies from headers; `http_set_cookie_header` builds `Set-Cookie` values with path, domain, max-age, Secure, HttpOnly, and SameSite options.

**Multipart form parsing** -- `http_multipart_parse` handles `multipart/form-data` with file uploads, field names, content types, and binary data.

**URL-encoded form parsing** -- `http_form_parse` handles `application/x-www-form-urlencoded` bodies with `http_form_get` for field lookup.

**Chunked transfer encoding** -- `http_chunked_write` and `http_chunked_read` for streaming large payloads.

**Request size limits** -- configurable via `http_set_limits`: max header size (default 8 KiB), max body size (default 1 MiB), and per-socket timeout (default 10s).

**Graceful shutdown** -- `http_shutdown` stops accepting new connections while in-flight requests complete (drain timeout 10s).

**HTTP client** -- `http_client` creates a connection pool with configurable timeout. Methods: `http_get`, `http_post`, `http_put`, `http_delete`.

**Streaming responses** -- `http_stream` opens a streaming connection; `http_streamnext` reads chunks incrementally.

### Hello World

A complete toke web server in 10 lines:

```toke
m=hello;
i=http:std.http;

f=home(req:http.$req):http.$res{
  < http.ok("Hello from toke!")
};

f=main():i64{
  http.get("/";home);
  < http.serve(8080 as u16)
};
```

Build and run:

```bash
tkc hello.tk -o hello
./hello
# In another terminal:
curl http://localhost:8080/
# Hello from toke!
```

### REST API Example

A four-route CRUD API for a to-do list:

```toke
m=todos;
i=http:std.http;

let store=mut.@($str);

f=list(req:http.$req):http.$res{
  < http.json(200 as u16;store.to_json())
};

f=create(req:http.$req):http.$res{
  let item=$str=req.body;
  store.push(item);
  < http.json(201 as u16;item)
};

f=show(req:http.$req):http.$res{
  let id=$str!=http.param(req;"id");
  if(id.is_err){< http.bad("missing id")};
  < http.json(200 as u16;store.get(id.ok))
};

f=remove(req:http.$req):http.$res{
  let id=$str!=http.param(req;"id");
  if(id.is_err){< http.bad("missing id")};
  store.remove(id.ok);
  < http.ok("deleted")
};

f=main():i64{
  http.get("/todos";list);
  http.post("/todos";create);
  http.get("/todos/:id";show);
  http.delete("/todos/:id";remove);
  < http.serve(8080 as u16)
};
```

```bash
tkc todos.tk -o todos && ./todos
```

### Bookmarks REST API

A more complete CRUD example with JSON request parsing, error handling, and all four HTTP methods:

```toke
m=bookmarks;
i=http:std.http;
i=json:std.json;

t=$bm{url:$str;title:$str;tags:@($str)};
let db=mut.@($str;$bm);
let next_id=mut.1;

f=list_all(req:http.$req):http.$res{
  < http.json(200 as u16;json.encode(db.values()))
};

f=create(req:http.$req):http.$res{
  let parsed=$bm!=json.decode(req.body;$bm);
  if(parsed.is_err){< http.bad("invalid JSON: expected {url,title,tags}")};
  let bm=$bm=parsed.ok;
  if(bm.url.len()==0){< http.bad("url is required")};
  let id=$str=next_id.to_str();
  next_id=next_id+1;
  db.set(id;bm);
  < http.json(201 as u16;json.encode(@("id"=>id;"url"=>bm.url;"title"=>bm.title)))
};

f=update(req:http.$req):http.$res{
  let id=$str!=http.param(req;"id");
  if(id.is_err){< http.bad("missing id")};
  if(db.has(id.ok)==false){< http.json(404 as u16;json.encode(@("error"=>"not found")))};
  let parsed=$bm!=json.decode(req.body;$bm);
  if(parsed.is_err){< http.bad("invalid JSON")};
  db.set(id.ok;parsed.ok);
  < http.json(200 as u16;json.encode(parsed.ok))
};

f=delete(req:http.$req):http.$res{
  let id=$str!=http.param(req;"id");
  if(id.is_err){< http.bad("missing id")};
  if(db.has(id.ok)==false){< http.json(404 as u16;json.encode(@("error"=>"not found")))};
  db.remove(id.ok);
  < http.json(200 as u16;json.encode(@("deleted"=>id.ok)))
};

f=main():i64{
  http.get("/api/bookmarks";list_all);
  http.post("/api/bookmarks";create);
  http.put("/api/bookmarks/:id";update);
  http.delete("/api/bookmarks/:id";delete);
  < http.serve(8080 as u16)
};
```

Test it:

```bash
tkc bookmarks.tk -o bookmarks && ./bookmarks &

# Create
curl -X POST http://localhost:8080/api/bookmarks \
  -H "Content-Type: application/json" \
  -d '{"url":"https://toke.dev","title":"toke","tags":["lang","compiler"]}'

# List all
curl http://localhost:8080/api/bookmarks

# Update
curl -X PUT http://localhost:8080/api/bookmarks/1 \
  -H "Content-Type: application/json" \
  -d '{"url":"https://toke.dev","title":"toke language","tags":["lang"]}'

# Delete
curl -X DELETE http://localhost:8080/api/bookmarks/1

# Error handling
curl -X POST http://localhost:8080/api/bookmarks \
  -d 'not json'
# {"error":"invalid JSON: expected {url,title,tags}"}
```

### Production Setup: TLS + Workers

A production-ready server with TLS termination, pre-fork workers, rate limiting, gzip compression, and cache headers:

```toke
m=prod;
i=http:std.http;

f=index(req:http.$req):http.$res{
  < http.ok("running")
};

f=main():i64{
  // Rate-limiting / gzip / cache-control are not in-runtime toke APIs — the
  // in-runtime WAF was removed (Story 121.13). Put those at an external reverse
  // proxy (nginx, Caddy, a cloud WAF). toke's controls are deny-by-default
  // capabilities (ADR-0010), auto-escaping/parameterized queries (ADR-0011), and
  // the spatial-safety traps (ADR-0012).
  http.get("/";index);

  < http.serve_tls_workers(
    443 as u16;
    "/etc/toke/cert.pem";
    "/etc/toke/key.pem";
    8 as u32
  )
};
```

Deploy:

```bash
# Build a static binary
tkc prod.tk -o prod --release

# Copy to server
scp prod user@server:/opt/toke/prod

# Run with systemd (create /etc/systemd/system/toke-prod.service)
# [Service]
# ExecStart=/opt/toke/prod
# Restart=always
# LimitNOFILE=65536

sudo systemctl enable toke-prod
sudo systemctl start toke-prod
```

The binary is ~2 MB. No runtime, no container image, no reverse proxy needed for TLS. Eight pre-fork workers share the listening socket via `SO_REUSEPORT`.

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

What can be stated without a benchmark: the binary is ~2 MB, it has no
runtime dependencies, it terminates TLS itself, and it pre-forks workers that
share the listening socket via `SO_REUSEPORT`.

### How it compares

Capabilities only — these are properties of each stack, not measurements.
The "hello world lines" and third-party binary sizes that stood in this table
are withdrawn with the throughput table above (132.38): they were counted from
unstated sources.

| | toke | Go net/http | Node Express | Rust actix-web |
|---|---|---|---|---|
| Runtime deps | 0 | 0 | 30+ (npm) | 0 |
| TLS built-in | Yes | Yes | No (needs proxy) | Yes |
| Worker pool built-in | Yes | Yes (goroutines) | No (needs cluster) | Yes |
| Package manager required | No | No | Yes (npm) | Yes (cargo) |
