# HTTP Handler Contract — Story 82.1.2

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

Two glue-level response constructors exist in `tk_web_glue.c`:

| Glue function | tki name | Signature | Effect |
|---------------|----------|-----------|--------|
| `tk_http_res_new` | `http.Res.ok` | `(body:str) -> Res` | 200 OK with `text/html` Content-Type |
| `tk_http_res_json_new` | `http.Res.json` | `(status:u16; body:str) -> Res` | Given status with `application/json` Content-Type |

Two additional response constructors are declared in `http.tki` and implemented in `http.c` (C-level, not yet wrapped in glue as i64):

| C function | tki name | Signature | Effect |
|------------|----------|-----------|--------|
| `http_Res_bad` | `http.Res.bad` | `(msg:str) -> Res` | 400 Bad Request |
| `http_Res_err` | `http.Res.err` | `(msg:str) -> Res` | 500 Internal Server Error |

> **Note:** `http.Res.bad` and `http.Res.err` exist in the C library and
> the tki schema but do not yet have i64-ABI wrappers in `tk_web_glue.c`.
> Use `http.Res.json(400; msg)` or `http.Res.json(500; msg)` as
> workarounds until glue wrappers are added.

---

## 3. Reading request data

Three request accessors have i64-ABI glue wrappers (Story 46.1.3):

| Glue function | Signature | Returns |
|---------------|-----------|---------|
| `tk_http_req_path(req:i64)` | `i64 -> i64` | URL path as `$str` (defaults to `"/"`) |
| `tk_http_req_method(req:i64)` | `i64 -> i64` | HTTP method as `$str` (defaults to `"GET"`) |
| `tk_http_req_body(req:i64)` | `i64 -> i64` | Request body as `$str` (defaults to `""`) |

Two additional accessors are declared in `http.tki` and implemented in `http.c`:

| C function | tki name | Signature | Returns |
|------------|----------|-----------|---------|
| `http_param` | `http.param` | `(Req, str) -> str!HttpErr` | URL parameter value by name |
| `http_header` | `http.header` | `(Req, str) -> str!HttpErr` | Header value by name |

> **Note:** `http.param` and `http.header` take a `Req` struct by value
> (not an i64 pointer). They work at the C level but do not yet have
> i64-ABI glue wrappers, so they are not callable from compiled toke
> handler functions. The `Req` struct fields (`path`, `method`, `body`)
> are accessible via the three glue wrappers above.

---

## 4. Registering handlers

Route registration glue functions (one per HTTP method):

| Glue function | tki name | Signature |
|---------------|----------|-----------|
| `tk_http_get_handler` | `http.GET` | `(path:str; handler:FuncDecl)` |
| `tk_http_post_handler` | `http.POST` | `(path:str; handler:FuncDecl)` |
| `tk_http_put_handler` | `http.PUT` | `(path:str; handler:FuncDecl)` |
| `tk_http_delete_handler` | `http.DELETE` | `(path:str; handler:FuncDecl)` |
| `tk_http_patch_handler` | `http.PATCH` | `(path:str; handler:FuncDecl)` |

Example in toke:

```
i=http:std.http;

f=helloget(req:i64):i64{
  <http.Res.json(200;"{\"msg\":\"hello\"}")
};

f=main():i64{
  (http.GET("/api/hello";&helloget));
  (http.serve_workers(8080;1));
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
