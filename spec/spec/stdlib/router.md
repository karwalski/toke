# std.router

HTTP request router with path parameters, method-based routing, middleware, and server lifecycle.

## Types

```
type router     { id: u64 }          -- opaque
type ctx        { req: http.Req, params: [[str]], state: [[str]] }
type handler    { id: u64 }          -- opaque
type middleware  { id: u64 }          -- opaque
type routererr  { msg: str }
```

## Functions

```
f=new():router
f=get(r:router;pattern:str;h:handler):void
f=post(r:router;pattern:str;h:handler):void
f=put(r:router;pattern:str;h:handler):void
f=delete(r:router;pattern:str;h:handler):void
f=use(r:router;mw:middleware):void
f=serve(r:router;host:str;port:u64):void!routererr
```

## Semantics

- `new` creates an empty router with no routes or middleware.
- `get`, `post`, `put`, `delete` register a handler for the given HTTP method and path pattern. Patterns support named segments (e.g., `"/users/:id"`).
- `ctx.params` contains captured path parameters as key-value pairs.
- `ctx.state` holds per-request state set by middleware.
- `use` adds middleware that runs before every matched handler, in registration order.
- `serve` binds to `host:port` and blocks, serving requests. Returns `routererr` if the address is already in use or binding fails.

## Dependencies

- `std.http` (request/response types)
