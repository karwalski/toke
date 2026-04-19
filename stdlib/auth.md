# std.auth -- Authentication and Authorization

## Overview

The `std.auth` module provides functions for JSON Web Token (JWT) signing and verification, API key generation and validation, and bearer token extraction from HTTP headers. All cryptographic operations delegate to `std.crypto`; this module handles the authentication-specific framing, claim structure, and error reporting.

JWTs follow RFC 7519. Tokens are compact strings with three Base64url-encoded segments (header, payload, signature). The module supports HMAC-SHA and RSA signing algorithms via the `$JwtAlg` sum type.

API keys are opaque, prefix-scoped strings suitable for machine-to-machine authentication. They are validated against an external `$Keystore` handle that the caller provisions.

## Types

### JwtAlg

A sum type selecting the signing algorithm for JWT operations.

| Variant | Meaning |
|---------|---------|
| Hs256   | HMAC-SHA256 symmetric signing |
| Hs384   | HMAC-SHA384 symmetric signing |
| Rs256   | RSASSA-PKCS1-v1_5 with SHA-256 (asymmetric) |

```toke
let alg = $JwtAlg.Hs256;
```

### JwtClaims

A record holding the standard JWT registered claims plus an extensible key-value bag.

| Field | Type     | Meaning |
|-------|----------|---------|
| sub   | str      | Subject -- the principal the token describes |
| iss   | str      | Issuer -- who created and signed the token |
| exp   | u64      | Expiration time as a Unix epoch timestamp (seconds) |
| iat   | u64      | Issued-at time as a Unix epoch timestamp (seconds) |
| extra | [[str]]  | Additional claims as an array of two-element string arrays (key-value pairs) |

```toke
let claims = $JwtClaims{
    sub   = "user-42";
    iss   = "api.example.com";
    exp   = 1719878400u64;
    iat   = 1719792000u64;
    extra = [["role"; "admin"]; ["tenant"; "acme"]];
};
```

### AuthErr

The error type returned by all fallible functions in this module.

| Field | Type | Meaning |
|-------|------|---------|
| msg   | str  | Human-readable description of the failure |
| code  | u64  | Numeric error code for programmatic handling |

Common codes: `1` -- malformed token, `2` -- signature mismatch, `3` -- expired token, `4` -- invalid key, `5` -- missing bearer prefix.

### Keystore

An opaque handle to an external API key store. The caller is responsible for opening and closing the store; this module only reads from it during validation.

| Field  | Type | Meaning |
|--------|------|---------|
| handle | u64  | Opaque reference to the backing store |

## Functions

### auth.jwtsign(claims: JwtClaims; secret: [byte]; alg: JwtAlg): str!AuthErr

Signs the given `claims` with `secret` using algorithm `alg` and returns a compact JWT string (three Base64url segments separated by dots). Returns `AuthErr` if the secret is empty, if the secret length is insufficient for the chosen algorithm, or if `alg` is `Rs256` and the secret is not a valid DER-encoded RSA private key.

The `exp` and `iat` fields are encoded as-is; the caller is responsible for setting them to sensible values before signing.

**Example:**
```toke
let secret = str.bytes("my-256-bit-secret-key-here!!");
let claims = $JwtClaims{
    sub   = "user-42";
    iss   = "api.example.com";
    exp   = 1719878400u64;
    iat   = 1719792000u64;
    extra = [];
};
let token = auth.jwtsign(claims; secret; $JwtAlg.Hs256);
(* token = ok("eyJhbGciOi...") *)
```

**Example -- error on empty secret:**
```toke
let bad = auth.jwtsign(claims; []; $JwtAlg.Hs256);
(* bad = err($AuthErr{msg = "secret must not be empty"; code = 4u64}) *)
```

### auth.jwtverify(token: str; secret: [byte]): JwtClaims!AuthErr

Parses the compact JWT string `token`, verifies its signature against `secret`, and returns the decoded `JwtClaims`. The algorithm is read from the token header; the caller does not need to specify it.

Returns `AuthErr` with code `1` if the token is malformed (wrong segment count, invalid Base64url), or code `2` if the signature does not match. This function does **not** check expiration; use `auth.jwtexpired` separately to enforce time-based validity.

**Example:**
```toke
let secret = str.bytes("my-256-bit-secret-key-here!!");
let result = auth.jwtverify(token; secret);
(* result = ok($JwtClaims{sub = "user-42"; ...}) *)
```

**Example -- tampered token:**
```toke
let bad = auth.jwtverify("eyJhbGciOi...tampered"; secret);
(* bad = err($AuthErr{msg = "signature mismatch"; code = 2u64}) *)
```

### auth.jwtexpired(claims: JwtClaims): bool

Returns `true` if `claims.exp` is less than or equal to the current Unix epoch time, `false` otherwise. A claims record with `exp = 0u64` is treated as never-expiring and always returns `false`.

This function is pure clock comparison with no I/O; it reads the system monotonic clock internally.

**Example:**
```toke
let claims = $JwtClaims{
    sub = "user-42"; iss = "api.example.com";
    exp = 1000u64; iat = 900u64; extra = [];
};
let expired = auth.jwtexpired(claims);  (* expired = true *)
```

**Example -- no expiry:**
```toke
let forever = $JwtClaims{
    sub = "service"; iss = "internal";
    exp = 0u64; iat = 1719792000u64; extra = [];
};
let ok = auth.jwtexpired(forever);  (* ok = false *)
```

### auth.apikeygenerate(prefix: str): str

Generates a new random API key string with the given `prefix` followed by a dot separator and a cryptographically random alphanumeric suffix (32 characters). The prefix allows visual identification of key purpose (e.g., `"pk_live"`, `"sk_test"`).

The returned key is always `len(prefix) + 1 + 32` bytes long. The prefix must not contain dots; if it does, the dot is silently stripped.

**Example:**
```toke
let key = auth.apikeygenerate("pk_live");
(* key = "pk_live.a8Xk9mQ2r7..." -- 40 chars total *)
```

**Example -- test prefix:**
```toke
let test_key = auth.apikeygenerate("sk_test");
(* test_key = "sk_test.Bz3nW8pL..." *)
```

### auth.apikeyvalidate(key: str; store: Keystore): bool!AuthErr

Looks up `key` in the external `store` and returns `true` if the key exists and is active, `false` if the key is not found. Returns `AuthErr` if the store handle is invalid or the store is unreachable (code `4`), or if the key string is malformed -- missing the dot separator or having an empty prefix (code `1`).

Validation is constant-time to prevent timing side-channel attacks.

**Example:**
```toke
let store  = get_keystore();  (* application-provided *)
let valid  = auth.apikeyvalidate("pk_live.a8Xk9mQ2r7..."; store);
(* valid = ok(true) *)
```

**Example -- malformed key:**
```toke
let bad = auth.apikeyvalidate("no-dot-here"; store);
(* bad = err($AuthErr{msg = "malformed key: missing separator"; code = 1u64}) *)
```

### auth.bearerextract(header: str): str!AuthErr

Extracts the token value from an HTTP `Authorization` header string. The header must begin with `"Bearer "` (case-sensitive, followed by exactly one space). Returns the remainder of the string after the prefix.

Returns `AuthErr` with code `5` if the header does not start with `"Bearer "`, or code `1` if the header is empty.

**Example:**
```toke
let token = auth.bearerextract("Bearer eyJhbGciOi...");
(* token = ok("eyJhbGciOi...") *)
```

**Example -- wrong scheme:**
```toke
let bad = auth.bearerextract("Basic dXNlcjpwYXNz");
(* bad = err($AuthErr{msg = "missing Bearer prefix"; code = 5u64}) *)
```

---

## Full Examples

### JWT issue-then-verify flow

```toke
(* Issue a token, then verify it on the receiving side *)

let secret = str.bytes("super-secret-256-bit-key-here!!!");

(* --- Issuer side --- *)
let claims = $JwtClaims{
    sub   = "user-42";
    iss   = "auth.example.com";
    exp   = 1719878400u64;
    iat   = 1719792000u64;
    extra = [["role"; "editor"]];
};
let token = try auth.jwtsign(claims; secret; $JwtAlg.Hs256);

(* --- Verifier side --- *)
let decoded = try auth.jwtverify(token; secret);

if auth.jwtexpired(decoded) {
    log("token expired -- reject request");
} else {
    log(str.concat("authenticated subject: "; decoded.sub));
};
```

### API key middleware pattern

```toke
(* Extract bearer token, then validate as API key *)

fn authenticate(header: str; store: Keystore): bool!AuthErr {
    let key   = try auth.bearerextract(header);
    let valid = try auth.apikeyvalidate(key; store);
    ret valid;
};

(* Usage in a request handler *)
let store  = get_keystore();
let result = authenticate(req.headers.authorization; store);

match result {
    ok(true)  => handle_request(req);
    ok(false) => respond(401u64; "invalid api key");
    err(e)    => respond(400u64; e.msg);
};
```
