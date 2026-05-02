---
title: std.crypto
slug: crypto
section: reference/stdlib
order: 6
---

**Status: Implemented** -- C runtime backing.

The `std.crypto` module provides cryptographic hash functions (SHA-256, SHA-512), HMAC message authentication codes, constant-time comparison, cryptographically secure random byte generation, and hex encoding.

> **Implemented functions (from `crypto.tki`):** `crypto.sha256`, `crypto.sha512`, `crypto.hmacsha256`, `crypto.hmacsha512`, `crypto.constanteq`, `crypto.randombytes`, `crypto.to_hex`. Functions documented in earlier versions (`crypto.md5`, `crypto.bcrypt_hash`, `crypto.bcrypt_verify`, `crypto.rand_bytes`, `crypto.constant_eq`, `crypto.hmac_sha256`, `crypto.hmac_sha512`, `crypto.from_hex`) used incorrect names or are not in the current tki.

## Functions

### crypto.sha256(data: @(byte)): @(byte)

Computes the SHA-256 digest of `data` and returns a 32-byte array containing the hash.

```toke
m=example;
i=crypto:std.crypto;
i=str:std.str;

f=hashstr():$str{
  let hash=crypto.sha256(str.tobytes("abc"));
  let hex=crypto.tohex(hash);
  < hex
};
```

### crypto.sha512(data: @(byte)): @(byte)

Computes the SHA-512 digest of `data` and returns a 64-byte array, offering a larger output than SHA-256 for applications that require it.

```toke
m=example;
i=crypto:std.crypto;
i=str:std.str;

f=hash512():$str{
  let hash=crypto.sha512(str.tobytes("abc"));
  let hex=crypto.tohex(hash);
  < hex
};
```

### crypto.hmacsha256(key: @(byte); data: @(byte)): @(byte)

Computes the HMAC-SHA-256 message authentication code using `key` and `data`, returning a 32-byte tag. Use this to authenticate messages between parties that share a secret key.

```toke
m=example;
i=crypto:std.crypto;
i=str:std.str;
i=env:std.env;

f=macdemo():$str{
  let secret=env.get_or("HMAC_SECRET";"fallback");
  let key=str.tobytes(secret);
  let payload=str.tobytes("amount=100&currency=usd");
  let tag=crypto.hmacsha256(key;payload);
  let hex=crypto.tohex(tag);
  < hex
};
```

### crypto.hmacsha512(key: @(byte); data: @(byte)): @(byte)

Computes the HMAC-SHA-512 message authentication code using `key` and `data`, returning a 64-byte tag. Prefer this over `hmacsha256` when a larger authentication tag is required by the protocol.

```toke
m=example;
i=crypto:std.crypto;
i=str:std.str;

f=mac512demo():i64{
  let key=str.tobytes("secret-key");
  let msg=str.tobytes("important payload");
  let tag=crypto.hmacsha512(key;msg);
  < 0
};
```

### crypto.constanteq(a: @(byte); b: @(byte)): bool

Compares two byte arrays in constant time, returning `true` only if both arrays have identical length and contents. Use this instead of plain equality when comparing secrets such as HMAC tags or tokens, as naive comparison leaks timing information that attackers can exploit.

```toke
m=example;
i=crypto:std.crypto;
i=str:std.str;

f=verifymac(keystr:$str;paystr:$str;providedstr:$str):bool{
  let key=str.tobytes(keystr);
  let payload=str.tobytes(paystr);
  let expected=crypto.hmacsha256(key;payload);
  let provided=str.tobytes(providedstr);
  let valid=crypto.constanteq(expected;provided);
  < valid
};
```

### crypto.randombytes(n: u64): @(byte)

Returns `n` cryptographically secure random bytes sourced from the operating system. Suitable for generating nonces, salts, session tokens, and other secrets.

```toke
m=example;
i=crypto:std.crypto;

f=gensalt():i64{
  let salt=crypto.randombytes(16);
  < 0
};
```

### crypto.to_hex(data: @(byte)): $str

Converts a byte array to a lowercase hexadecimal string, producing exactly two characters per byte.

```toke
m=example;
i=crypto:std.crypto;
i=str:std.str;

f=tohexdemo():$str{
  let hash=crypto.sha256(str.tobytes("abc"));
  let hex=crypto.tohex(hash);
  < hex
};
```

## Usage Examples

Generate a random session token and authenticate a webhook payload with HMAC-SHA-256:

```toke
m=cryptodemo;
i=crypto:std.crypto;
i=str:std.str;
i=env:std.env;

f=gentoken():$str{
  let bytes=crypto.randombytes(32);
  let token=crypto.tohex(bytes);
  < token
};

f=verifywebhook(body:$str;sigheader:$str):bool{
  let secret=env.get_or("WEBHOOK_SECRET";"");
  let key=str.tobytes(secret);
  let payload=str.tobytes(body);
  let expected=crypto.hmacsha256(key;payload);
  let provided=str.tobytes(sigheader);
  let valid=crypto.constanteq(expected;provided);
  < valid
};
```

## See Also

- [std.encrypt](/docs/stdlib/encrypt) -- AES-256-GCM encryption, X25519 key exchange, Ed25519 signatures, and HKDF key derivation
