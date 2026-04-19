# Cookbook: Authenticated API with JWT

## Overview

Build an API server with JWT authentication, protected routes, and a login endpoint. Combines `std.http`, `std.auth`, and `std.router` to show the standard pattern for securing web APIs in toke.

## Modules Used

- `std.http` -- HTTP server and request/response handling
- `std.auth` -- JWT creation, verification, and middleware
- `std.router` -- Route grouping and middleware chaining

## Complete Program

```toke
I std.http;
I std.auth;
I std.router;
I std.json;
I std.str;

(* Secret key for signing JWTs -- load from env in production *)
let secret = "change-me-in-production";

(* Configure the JWT signer *)
let jwt = auth.new_jwt(auth.JwtConfig{
    secret: secret;
    algorithm: auth.HS256;
    expiry_secs: 3600;
});

(* Middleware: verify JWT and attach claims to request *)
fn require_auth(req: http.Request; next: fn(http.Request): http.Response): http.Response {
    let header = http.get_header(req; "Authorization")!;
    let token = str.trim_prefix(header; "Bearer ")!;
    let claims = auth.verify(jwt; token) catch |err| {
        ret http.respond(401; "{ \"error\": \"invalid token\" }"; "application/json");
    };
    let enriched = http.set_context(req; "user_id"; auth.claim(claims; "sub"));
    next(enriched);
};

(* Public route: login and receive a token *)
fn login(req: http.Request): http.Response {
    let body = json.decode(req.body)!;
    let username = json.get_str(body; "username")!;
    let password = json.get_str(body; "password")!;

    (* In production, verify against a database *)
    if username != "admin" {
        ret http.respond(401; "{ \"error\": \"bad credentials\" }"; "application/json");
    };
    if password != "secret" {
        ret http.respond(401; "{ \"error\": \"bad credentials\" }"; "application/json");
    };

    let claims = auth.new_claims();
    auth.set_claim(claims; "sub"; "user_1");
    auth.set_claim(claims; "role"; "admin");
    let token = auth.sign(jwt; claims)!;

    let result = json.new_object();
    json.set(result; "token"; json.from_str(token));
    http.respond(200; json.encode(result); "application/json");
};

(* Protected route: return the current user profile *)
fn get_profile(req: http.Request): http.Response {
    let user_id = http.get_context(req; "user_id")!;
    let result = json.new_object();
    json.set(result; "user_id"; json.from_str(user_id));
    json.set(result; "status"; json.from_str("authenticated"));
    http.respond(200; json.encode(result); "application/json");
};

(* Protected route: admin-only resource *)
fn list_users(req: http.Request): http.Response {
    let result = json.new_array();
    json.push(result; json.from_str("user_1"));
    json.push(result; json.from_str("user_2"));
    http.respond(200; json.encode(result); "application/json");
};

(* Build the router *)
let r = router.new();

(* Public routes *)
router.post(r; "/login"; login);

(* Protected route group with auth middleware *)
let api = router.group(r; "/api"; require_auth);
router.get(api; "/profile"; get_profile);
router.get(api; "/users"; list_users);

(* Start the server *)
let server = http.listen(8080)!;
router.mount(server; r);
http.serve(server);
```

## Step-by-Step Explanation

1. **JWT configuration** -- `auth.new_jwt` creates a signer with a secret, algorithm, and expiry. HS256 is a symmetric HMAC algorithm suitable for single-server deployments.

2. **Auth middleware** -- `require_auth` extracts the Bearer token from the Authorization header, verifies it, and attaches the user claims to the request context. Invalid tokens receive a 401 immediately.

3. **Login handler** -- Parses credentials from the JSON body, validates them, creates claims with a subject and role, and signs a JWT. The token is returned as JSON.

4. **Protected handlers** -- `get_profile` and `list_users` read the user_id from the request context set by the middleware. They only execute if the JWT was valid.

5. **Router setup** -- `router.group` creates a route group with shared middleware. All routes under `/api` pass through `require_auth` before reaching their handler.

6. **Server startup** -- `router.mount` attaches the router to the HTTP server. `http.serve` blocks and processes incoming requests.
