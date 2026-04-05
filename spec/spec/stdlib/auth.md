# std.auth

JWT signing/verification, API key generation/validation, and bearer token extraction.

## Types

```
sum JwtAlg = Hs256 | Hs384 | Rs256

type JwtClaims { sub: str, iss: str, exp: u64, iat: u64, extra: [[str]] }
type AuthErr   { msg: str, code: u64 }
type Keystore  { handle: u64 }
```

## Functions

```
f=jwtsign(claims:JwtClaims;secret:[byte];alg:JwtAlg):str!AuthErr
f=jwtverify(token:str;secret:[byte]):JwtClaims!AuthErr
f=jwtexpired(claims:JwtClaims):bool
f=apikeygenerate(prefix:str):str
f=apikeyvalidate(key:str;store:Keystore):bool!AuthErr
f=bearerextract(header:str):str!AuthErr
```

## Semantics

- `jwtsign` encodes the header and claims as Base64URL JSON and signs with the given algorithm. Returns `AuthErr` if the key is invalid for the chosen algorithm.
- `jwtverify` validates the signature, decodes the payload, and returns claims. Returns `AuthErr` on invalid signature, malformed token, or unsupported algorithm.
- `jwtexpired` compares `claims.exp` against the current wall-clock time.
- `apikeygenerate` produces a cryptographically random key with the given prefix (e.g., `"tk_live"`).
- `apikeyvalidate` looks up the key in the `Keystore`. Returns `AuthErr` on store access failure.
- `bearerextract` parses `"Bearer <token>"` from an Authorization header value. Returns `AuthErr` if the header is missing or malformed.

## Dependencies

- `std.encoding` (Base64URL)
- `std.crypto` (HMAC, random bytes)
