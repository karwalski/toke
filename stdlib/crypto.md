# std.crypto -- Cryptographic Hash Functions

## Overview

The `std.crypto` module provides cryptographic hash and HMAC functions. The implementation is self-contained with no external dependencies beyond libc. All functions are infallible.

## Functions

### crypto.sha256(data: [Byte]): [Byte]

Computes the SHA-256 digest of `data`. Returns a 32-byte array containing the hash.

**Example:**
```toke
let hash = crypto.sha256(str.bytes("abc"));
let hex = crypto.to_hex(hash);
(* hex = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad" *)
```

### crypto.hmac_sha256(key: [Byte]; data: [Byte]): [Byte]

Computes the HMAC-SHA-256 message authentication code using the given `key` and `data`. Returns a 32-byte array containing the tag.

**Example:**
```toke
let key = str.bytes("key");
let msg = str.bytes("The quick brown fox jumps over the lazy dog");
let tag = crypto.hmac_sha256(key; msg);
let hex = crypto.to_hex(tag);
(* hex = "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8" *)
```

### crypto.to_hex(data: [Byte]): Str

Converts a byte array to a lowercase hexadecimal string (2 characters per byte).

**Example:**
```toke
let hash = crypto.sha256(str.bytes("abc"));
let hex = crypto.to_hex(hash);
(* hex is a 64-character lowercase hex string *)

let single = crypto.to_hex([0xde]);
(* single = "de" *)

let empty = crypto.to_hex([]);
(* empty = "" *)
```
