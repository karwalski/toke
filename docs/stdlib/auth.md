---
title: std.auth
slug: auth
section: reference/stdlib
order: 3
---

**Status: Implemented** -- C runtime backing.

The `std.auth` module provides authentication primitives: JWT creation and validation, API key generation, and bearer token extraction from HTTP headers.

## Types

JWT signing is **HS256 only**: `auth.jwtsign` and `auth.jwtverify` take no
algorithm argument, and no algorithm type is declared. A token whose header
names any other algorithm fails `auth.jwtverify`.

### $jwtclaims

| Field | Type | Meaning |
|-------|------|---------|
| sub | $str | Subject identifier |
| iss | $str | Issuer |
| iat | u64 | Issued-at time (Unix epoch seconds) |
| exp | u64 | Expiration time (Unix epoch seconds) |
| aud | $str | Audience |

### $autherr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |
| code | u64 | Error code |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `auth.jwtsign` | `claims: $jwtclaims; secret: @($byte)` | `$str!$autherr` | Sign claims into a JWT string (HS256; there is no algorithm parameter) |
| `auth.jwtverify` | `token: $str; secret: @($byte)` | `$jwtclaims!$autherr` | Verify a JWT's signature and validity window and extract its claims. Fails when the current time is at or after `exp`, before `nbf`, or when either claim is present but not an integer |
| `auth.jwtexpired` | `token: $str` | `bool` | Check whether a JWT token's `exp` claim has passed (`true` for a malformed token) |
| `auth.apikeygenerate` | (none) | `$str` | Generate a random API key (32 random bytes, base64url-encoded) |
| `auth.apikeyvalidate` | `provided: $str; stored: $str` | `bool` | Compare a presented API key with the stored one; `true` when they match |
| `auth.bearerextract` | `header: $str` | `$str!$autherr` | Extract the token from a `Bearer <token>` header value |
| `auth.passwordhash` | `password: $str` | `$str!$autherr` | Hash a password with bcrypt (cost 12) |
| `auth.passwordverify` | `password: $str; hash: $str` | `bool` | Check a password against a bcrypt hash; `false` on mismatch or a malformed hash |

## Usage

```toke
m=example;
i=auth:std.auth;
i=str:std.str;

f=main():i64{
  let key = str.bytes("mysecret");
  let apikey = auth.apikeygenerate();
  let header = "Bearer sometoken";
  let tok = mt auth.bearerextract(header) {$ok:t t;$err:e ""};
  <0;
}
```

## Dependencies

- `std.encoding` -- Base64 encoding for JWT segments.
- `std.crypto` -- HMAC and hash functions.
