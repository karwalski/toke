---
title: HTTP Handler Contract
slug: http-handlers
section: reference
order: 24
---

Reference for writing dynamic HTTP handlers in toke, backed by
`std.http` (see `stdlib/http.tki` and `src/stdlib/tk_web_glue.c`).

---

## 1. Handler signature

```
f=myhandler(req:i64):i64
```

| Part | Meaning |
|------|---------|
| `req` parameter | i64 pointer to the C `Req` struct (fields: `method`, `path`, `body`, `headers`, `params`) |
| Return value | i64 pointer to a heap-allocated C `Res` struct (fields: `status`, `body`, `headers`) |

The C structs are defined in `src/stdlib/http.h`.

---

## 2. Building responses

An `i64` handler returns one of four response constructors declared in
`stdlib/http.tki` and backed by glue in `tk_web_glue.c`:

| Glue function | tki name | Signature | Effect |
|---------------|----------|-----------|--------|
| `tk_http_res_ok` | `http.resok` | `(body:str) -> i64` | 200 OK with `text/html` Content-Type |
| `tk_http_res_json_new` | `http.resjson` | `(status:i64; body:str) -> i64` | Given status with `application/json` Content-Type |
| `tk_http_res_bad` | `http.resbad` | `(msg:str) -> i64` | 400 Bad Request, `text/plain` |
| `tk_http_res_err` | `http.reserr` | `(msg:str) -> i64` | 500 Internal Server Error, `text/plain` |

The typed `$res` forms, `http.res.ok`, `http.res.json`, `http.res.bad` and
`http.res.err`, are documented in `docs/stdlib/http.md` § Response
Constructors. All names are lower case: upper case is outside toke's
character set, so `http.Res.json` is rejected at parse time. (This section used to name the
upper-case `http.Res.*` forms and say `bad`/`err` had no `i64` wrappers; both
were stale -- 132.133.)

---

## 3. Reading request data

Three request accessors have i64-ABI glue wrappers (Story 46.1.3):

| Glue function | Signature | Returns |
|---------------|-----------|---------|
| `tk_http_req_path(req:i64)` | `i64 -> i64` | URL path as `$str` (defaults to `"/"`) |
| `tk_http_req_method(req:i64)` | `i64 -> i64` | HTTP method as `$str` (defaults to `"GET"`) |
| `tk_http_req_body(req:i64)` | `i64 -> i64` | Request body as `$str` (defaults to `""`) |

Two fallible accessors are declared in `http.tki` and callable from toke
handlers through their glue wrappers in `tk_web_glue.c`:

| Glue function | tki name | Signature | Returns |
|---------------|----------|-----------|---------|
| `tk_http_param_w(req:i64; name:i64)` | `http.param` | `(req; str) -> str!httprouteerr` | URL parameter value by name; `$notfound` when the route captured none by that name |
| `tk_http_header_w(req:i64; name:i64)` | `http.header` | `(req; str) -> str!httprouteerr` | Header value by name (case-insensitive); `$notfound` when absent |

`http.reqparam` / `http.reqheader` are the plain-`str` forms (`""` when
missing).  See `docs/stdlib/http.md` for the full contract; conformance
C340 and C362 drive both from compiled toke.  (This section used to say
`http.param`/`http.header` had no glue and were not callable from toke, with
an `HttpErr` error type; both were stale -- 135.31.)

---

## 4. Registering handlers

Route registration glue functions (one per HTTP method):

| Glue function | tki name | Signature |
|---------------|----------|-----------|
| `tk_http_get_handler` | `http.get` | `(path:str; handler:FuncDecl)` |
| `tk_http_post_handler` | `http.post` | `(path:str; handler:FuncDecl)` |
| `tk_http_put_handler` | `http.put` | `(path:str; handler:FuncDecl)` |
| `tk_http_delete_handler` | `http.delete` | `(path:str; handler:FuncDecl)` |
| `tk_http_patch_handler` | `http.patch` | `(path:str; handler:FuncDecl)` |

Example in toke:

```
i=http:std.http;

f=helloget(req:i64):i64{
  <http.resjson(200;"{\"msg\":\"hello\"}")
};

f=main():i64{
  http.get("/api/hello";&helloget);
  http.serveworkers(8080;1);
  <0
};
```

---

## 5. Server startup

| tki name | Glue exists | Signature | Notes |
|----------|-------------|-----------|-------|
| `http.serve_workers` | Yes | `(port:u64; workers:u64) -> void` | Start pre-fork worker pool; workers=1 is single-process |
| `http.serve_tls` | Yes | `(port:u64; cert_path:str; key_path:str) -> void` | HTTPS with TLS termination |
| `http.setcors` | Yes (`tk_http_set_cors`) | `(origins:str) -> i64` | Set `Access-Control-Allow-Origin` for all responses |
| `http.getstaticmime` | Yes (`tk_http_get_static_mime`) | `(path:str; body:str; mime_type:str) -> i64` | Register static GET route with explicit Content-Type |

---

## 6. With ooke serve

When using ooke's `serve.tk`, register dynamic handlers before calling
`serve_workers` or `serve_tls`. Add each handler's path to the
`handledpaths` array so that ooke's static-file fallback skips those
routes.

---

## Source files

- `stdlib/http.tki` — module schema (types, function signatures)
- `src/stdlib/http.h` — C struct definitions (`Req`, `Res`, `HttpErr`, `HttpResult`)
- `src/stdlib/http.c` — C implementations of response constructors and accessors
- `src/stdlib/tk_web_glue.c` — i64-ABI wrappers linking compiled toke to C stdlib
