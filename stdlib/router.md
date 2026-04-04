# std.router -- HTTP Routing with Middleware

## Overview

The `std.router` module provides a structured HTTP routing layer built on top of `std.http`. Routes are registered for specific HTTP verbs and URL patterns, including named path parameters (e.g., `/items/:id`). Middleware can be stacked to handle cross-cutting concerns such as authentication, logging, CORS, and rate limiting. The router blocks on `router.serve()` until the process exits or an error occurs.

## Types

### router

An opaque handle representing a configured router instance. Created by `router.new()`.

### ctx

The request context passed to each handler.

| Field | Type | Meaning |
|-------|------|---------|
| req | http.Req | The incoming HTTP request |
| params | [[Str]] | Named path parameters extracted from the URL pattern |
| state | [[Str]] | Key-value state set by middleware earlier in the chain |

### handler

An opaque reference to a registered route handler (closure workaround). Construct with `$handler{id: u64}`.

### middleware

An opaque reference to a registered middleware function. Construct with `$middleware{id: u64}`.

### routererr

Returned when `router.serve()` fails to bind or encounters a fatal error.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable error description |

## Functions

### router.new(): router

Creates a new, empty router with no routes or middleware registered.

**Example:**
```toke
let r = router.new();
```

### router.get(r: router; path: Str; h: handler): void

Registers a handler for GET requests matching `path`. Path segments prefixed with `:` are captured as named parameters accessible via `ctx.params`.

**Example:**
```toke
let h = $handler{id: 1};
router.get(r; "/items/:id"; h);
```

### router.post(r: router; path: Str; h: handler): void

Registers a handler for POST requests.

### router.put(r: router; path: Str; h: handler): void

Registers a handler for PUT requests.

### router.delete(r: router; path: Str; h: handler): void

Registers a handler for DELETE requests.

### router.use(r: router; m: middleware): void

Registers a middleware that runs before every route handler. Middleware are executed in registration order.

**Example:**
```toke
let mw = $middleware{id: 10};
router.use(r; mw);
```

### router.serve(r: router; addr: Str; port: u64): void!routererr

Binds to `addr:port` and begins accepting connections. Blocks until the process exits or a fatal error occurs.

**Example:**
```toke
let result = router.serve(r; "0.0.0.0"; 8080);
match result {
  ok(_)   { log.info("server stopped") };
  err(e)  { log.error(e.msg) };
};
```

## Error Types

### routererr

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Description of the bind or listen failure |
