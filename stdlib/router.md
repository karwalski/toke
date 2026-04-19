# std.router -- HTTP Routing with Middleware

## Overview

The `std.router` module provides a structured HTTP routing layer built on top of `std.http`. Routes are registered for specific HTTP verbs and URL patterns, including named path parameters (e.g., `/items/:id`). Middleware can be stacked to handle cross-cutting concerns such as authentication, logging, CORS, and rate limiting. The router blocks on `router.serve()` until the process exits or an error occurs.

Typical usage follows a three-step pattern: create a router, register middleware and routes, then call `router.serve()` to start accepting connections.

## Types

### router

An opaque handle representing a configured router instance. Created by `router.new()`. The handle is passed as the first argument to every function in this module.

### ctx

The request context passed to each handler and middleware function. Contains the original HTTP request, any named path parameters extracted from the URL pattern, and a key-value state map that middleware can use to pass data down the chain.

| Field | Type | Meaning |
|-------|------|---------|
| req | http.Req | The incoming HTTP request (method, path, headers, body) |
| params | [[Str]] | Named path parameters extracted from the URL pattern |
| state | [[Str]] | Key-value state set by middleware earlier in the chain |

Path parameters are populated from `:name` segments in the route pattern. For a route registered as `/users/:id/posts/:pid`, a request to `/users/42/posts/7` produces `params = [["id"; "42"]; ["pid"; "7"]]`.

State entries are string pairs set by middleware via the runtime. For example, an auth middleware might set `[["user_id"; "123"]]` so downstream handlers can read the authenticated user.

### handler

An opaque reference to a registered route handler (closure workaround). Construct with `$handler{id: u64}`. The `id` maps to a handler function registered with the runtime.

### middleware

An opaque reference to a registered middleware function. Construct with `$middleware{id: u64}`. The `id` maps to a middleware function registered with the runtime.

### routererr

Returned when `router.serve()` fails to bind or encounters a fatal error.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable error description |

## Functions

### router.new(): router

Creates a new, empty router with no routes or middleware registered. The returned handle is used with all subsequent registration and serving calls.

**Example:**
```toke
let r = router.new();
```

---

### router.get(r: router; path: Str; h: handler): void

Registers a handler for HTTP GET requests matching `path`. Path segments prefixed with `:` become named parameters accessible via `ctx.params` inside the handler.

GET routes are typically used for read operations: fetching a resource, listing a collection, or serving static content.

**Example -- static path:**
```toke
let list_items = $handler{id: 1};
router.get(r; "/items"; list_items);
```

**Example -- parameterised path:**
```toke
let get_item = $handler{id: 2};
router.get(r; "/items/:id"; get_item);
(* A request to /items/42 sets ctx.params = [["id"; "42"]] *)
```

---

### router.post(r: router; path: Str; h: handler): void

Registers a handler for HTTP POST requests matching `path`. POST routes are used for creating new resources. The request body is available via `ctx.req`.

**Example:**
```toke
let create_item = $handler{id: 3};
router.post(r; "/items"; create_item);
(* Handler reads ctx.req.body to parse the new item payload *)
```

**Example -- nested resource:**
```toke
let add_comment = $handler{id: 4};
router.post(r; "/items/:id/comments"; add_comment);
(* ctx.params = [["id"; "..."]] plus ctx.req.body for the comment *)
```

---

### router.put(r: router; path: Str; h: handler): void

Registers a handler for HTTP PUT requests matching `path`. PUT routes are used for full replacement of an existing resource. The request body contains the complete updated representation.

**Example:**
```toke
let update_item = $handler{id: 5};
router.put(r; "/items/:id"; update_item);
(* Handler reads ctx.params for the id and ctx.req.body for the payload *)
```

**Example -- idempotent upsert:**
```toke
let upsert_config = $handler{id: 6};
router.put(r; "/config/:key"; upsert_config);
(* Creates the config entry if missing, replaces it if present *)
```

---

### router.delete(r: router; path: Str; h: handler): void

Registers a handler for HTTP DELETE requests matching `path`. DELETE routes remove a resource identified by the path.

**Example:**
```toke
let remove_item = $handler{id: 7};
router.delete(r; "/items/:id"; remove_item);
```

**Example -- bulk delete via parent:**
```toke
let clear_comments = $handler{id: 8};
router.delete(r; "/items/:id/comments"; clear_comments);
(* Removes all comments for the given item *)
```

---

### router.use(r: router; m: middleware): void

Registers a global middleware that runs before every route handler. Middleware are executed in registration order. Each middleware can inspect or modify the request context, set state entries for downstream handlers, or short-circuit the chain by returning an error response directly.

Call `router.use()` multiple times to build a middleware stack. The order of registration determines execution order: the first middleware registered runs first.

**Example -- single middleware:**
```toke
let logger = $middleware{id: 10};
router.use(r; logger);
```

**Example -- stacking multiple middleware:**
```toke
let cors    = $middleware{id: 11};
let auth    = $middleware{id: 12};
let limiter = $middleware{id: 13};

router.use(r; cors);
router.use(r; auth);
router.use(r; limiter);
(* Execution order: cors -> auth -> limiter -> route handler *)
```

---

### router.serve(r: router; addr: Str; port: u64): void!routererr

Binds to `addr:port` and begins accepting HTTP connections. This call blocks until the process exits or a fatal error occurs.

Returns `routererr` if the address/port cannot be bound (e.g., port already in use, invalid address, or insufficient permissions). Once successfully bound, the router dispatches incoming requests through the middleware chain and then to the matching route handler. Requests that match no registered route receive a 404 response automatically.

**Parameters:**

| Name | Type | Meaning |
|------|------|---------|
| r | router | The configured router instance |
| addr | Str | IP address to bind (e.g., `"0.0.0.0"` for all interfaces, `"127.0.0.1"` for localhost only) |
| port | u64 | TCP port number (e.g., `8080`) |

**Error cases:**

- Port already in use by another process.
- Binding to a privileged port (below 1024) without root permissions.
- Invalid IP address string.

**Example -- basic startup:**
```toke
let result = router.serve(r; "0.0.0.0"; 8080);
match result {
  ok(_)   { log.info("server stopped") };
  err(e)  { log.error(e.msg) };
};
```

**Example -- localhost-only binding:**
```toke
let result = router.serve(r; "127.0.0.1"; 3000);
match result {
  ok(_)   { log.info("dev server stopped") };
  err(e)  { log.error(str.concat("bind failed: "; e.msg)) };
};
```

## Error Types

### routererr

Returned by `router.serve()` when the server cannot start or encounters a fatal runtime error.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Description of the bind or listen failure |

**Common error messages:**

- `"address already in use"` -- another process holds the port.
- `"permission denied"` -- insufficient privileges for the requested port.
- `"invalid address"` -- the addr string could not be parsed as an IP address.

---

## Full Example -- REST API with Middleware

The following example wires up a complete item-management API with CORS, authentication, and logging middleware, plus CRUD route handlers.

```toke
(* --- Import modules --- *)
use std.router;
use std.http;
use std.log;
use std.str;

(* --- Create the router --- *)
let r = router.new();

(* --- Register middleware (runs in order) --- *)
let cors_mw = $middleware{id: 1};
let auth_mw = $middleware{id: 2};
let log_mw  = $middleware{id: 3};

router.use(r; cors_mw);   (* Set CORS headers on every response *)
router.use(r; auth_mw);   (* Validate bearer token, set ctx.state user_id *)
router.use(r; log_mw);    (* Log method, path, and response status *)

(* --- Register route handlers --- *)
let list_items   = $handler{id: 10};
let get_item     = $handler{id: 11};
let create_item  = $handler{id: 12};
let update_item  = $handler{id: 13};
let delete_item  = $handler{id: 14};

router.get(r;    "/api/items";     list_items);
router.get(r;    "/api/items/:id"; get_item);
router.post(r;   "/api/items";     create_item);
router.put(r;    "/api/items/:id"; update_item);
router.delete(r; "/api/items/:id"; delete_item);

(* --- Nested resource: comments on items --- *)
let list_comments  = $handler{id: 20};
let add_comment    = $handler{id: 21};

router.get(r;  "/api/items/:id/comments"; list_comments);
router.post(r; "/api/items/:id/comments"; add_comment);

(* --- Start serving --- *)
let result = router.serve(r; "0.0.0.0"; 8080);
match result {
  ok(_)  { log.info("server shut down cleanly") };
  err(e) { log.error(str.concat("server failed: "; e.msg)) };
};
```

**Request flow for `GET /api/items/42`:**

1. `cors_mw` -- adds `Access-Control-Allow-Origin` header.
2. `auth_mw` -- validates the `Authorization` header; sets `ctx.state = [["user_id"; "99"]]`.
3. `log_mw` -- records the request method and path.
4. `get_item` handler -- reads `ctx.params` to find `[["id"; "42"]]`, fetches the item, returns a JSON response.

Unmatched paths (e.g., `GET /unknown`) receive an automatic 404 with no handler invocation.
