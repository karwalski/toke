---
title: std.auth
slug: auth
section: reference/stdlib
order: 3
---

**Status: Implemented** -- C runtime backing.

The `std.auth` module provides authentication primitives: JWT creation and validation, API key generation, and bearer token extraction from HTTP headers.

## Types

### $jwtalg (sum type)

| Variant | Meaning |
|---------|---------|
| $hs256 | HMAC-SHA256 |
| $hs384 | HMAC-SHA384 |
| $rs256 | RSA-SHA256 |

### $jwtclaims

| Field | Type | Meaning |
|-------|------|---------|
| sub | $str | Subject identifier |
| iss | $str | Issuer |
| exp | u64 | Expiration time (Unix epoch seconds) |
| iat | u64 | Issued-at time (Unix epoch seconds) |
| extra | @(@($str)) | Additional key-value claim pairs |

### $autherr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |
| code | u64 | Error code |

### $keystore

| Field | Type | Meaning |
|-------|------|---------|
| handle | u64 | Opaque handle to a key store |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `auth.jwtsign` | `claims: $jwtclaims; secret: @($byte); alg: $jwtalg` | `$str!$autherr` | Sign claims into a JWT string |
| `auth.jwtverify` | `token: $str; secret: @($byte)` | `$jwtclaims!$autherr` | Verify a JWT and extract its claims |
| `auth.jwtexpired` | `claims: $jwtclaims` | `bool` | Check whether a JWT's claims have expired |
| `auth.apikeygenerate` | `prefix: $str` | `$str` | Generate a random API key with the given prefix |
| `auth.apikeyvalidate` | `key: $str; store: $keystore` | `bool!$autherr` | Validate an API key against a key store |
| `auth.bearerextract` | `header: $str` | `$str!$autherr` | Extract the token from a `Bearer <token>` header value |

## Usage

```toke
m=example;
i=auth:std.auth;
i=str:std.str;

f=main():i64{
  let key = str.bytes("mysecret");
  let apikey = auth.apikeygenerate("app");
  let header = "Bearer sometoken";
  let tok = auth.bearerextract(header)|{$ok:t t;$err:e ""};
  <0;
}
```

## Dependencies

- `std.encoding` -- Base64 encoding for JWT segments.
- `std.crypto` -- HMAC and hash functions.
