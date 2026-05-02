---
title: std.crypto — Extended Hashing and HMAC
slug: crypto_ext
section: reference/stdlib
order: 7
---

## Overview

This document covers the functions added in story 12.1.3 that extend the base
`std.crypto` module. The additions provide SHA-512 hashing, HMAC variants for
both SHA-256 and SHA-512, timing-safe byte comparison, and cryptographically
secure random byte generation.

The implementation uses CommonCrypto on macOS and libcrypto (OpenSSL) on Linux.
All functions operate on raw `@($byte)` slices rather than strings to avoid
encoding ambiguity and to allow zero-copy use with network buffers.

## Functions

### crypto.sha256(data: @($byte)): @($byte)

Computes the SHA-256 digest of `data`. Returns a 32-byte slice.

This function was already present in the base module (operating on `@($byte)`).
The interface is unchanged; the type annotation is updated to use `@($byte)` in
line with the default-syntax array notation.

**Example:**
```toke
let hash=crypto.sha256(str.bytes("hello"));
let hex=crypto.to_hex(hash);
-- hex = "2cf24dba5fb0a30e26e83b2ac5b9e29e1b161e5c1fa7425e73043362938b9824"
```

### crypto.sha512(data: @($byte)): @($byte)

Computes the SHA-512 digest of `data`. Returns a 64-byte slice.

**Example:**
```toke
let hash=crypto.sha512(str.bytes("hello"));
let hex=crypto.to_hex(hash);
-- hex is a 128-character lowercase hex string
```

### crypto.hmacsha256(key: @($byte); data: @($byte)): @($byte)

Computes HMAC-SHA-256 using `key` over `data`. Returns a 32-byte authentication
tag. The key may be any length; keys shorter than 64 bytes are zero-padded
internally per RFC 2104.

**Example:**
```toke
let key=str.bytes("secret");
let msg=str.bytes("payload");
let tag=crypto.hmacsha256(key;msg);
let hex=crypto.to_hex(tag);
```

### crypto.hmacsha512(key: @($byte); data: @($byte)): @($byte)

Computes HMAC-SHA-512 using `key` over `data`. Returns a 64-byte authentication
tag.

**Example:**
```toke
let key=str.bytes("secret");
let msg=str.bytes("payload");
let tag=crypto.hmacsha512(key;msg);
(test.eq(tag.len;64));
```

### crypto.constanteq(a: @($byte); b: @($byte)): bool

Compares two byte slices in constant time. Returns `true` if and only if `a`
and `b` have the same length and the same content. The comparison time is
proportional to `a.len` and does not short-circuit on mismatch, preventing
timing side-channel attacks.

Use this function when verifying HMAC tags or password hashes. Do **not** use
the built-in `==` operator for that purpose, as it may short-circuit.

**Example:**
```toke
let ok=crypto.constanteq(expected_tag;received_tag);
if !ok{
  <err("tag mismatch")
};
```

### crypto.randombytes(n: u64): @($byte)

Returns `n` cryptographically random bytes sourced from the operating system
entropy pool (`arc4random_buf` on macOS, `getrandom` on Linux). This function
is suitable for generating nonces, salts, session tokens, and API keys.

**Example:**
```toke
let nonce=crypto.randombytes(16);
let nonce_hex=crypto.to_hex(nonce);
-- nonce_hex is a 32-character random hex string
```

## Usage Pattern — HMAC API Key Verification

```toke
m=example;
i=crypto:std.crypto;

f=verifyrequest(key:@(u8);body:@(u8);receivedsig:@(u8)):bool{
  let expected=crypto.hmacsha256(key;body);
  <crypto.constanteq(expected;receivedsig)
};

f=signpayload(key:@(u8);payload:@(u8)):@(u8){
  <crypto.hmacsha256(key;payload)
};
```

## Notes

- All hash and HMAC functions are infallible — they do not return error variants.
- `crypto.randombytes` panics on entropy source failure (extremely rare, OS-level fault).
- `crypto.to_hex` (from the base module) is the canonical way to produce a
  printable representation of any `@($byte)` digest.
