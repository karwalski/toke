# std.encoding

Binary-to-text encoding and decoding: Base64, Base64URL, hex, and URL percent-encoding.

## Types

```
type EncodingErr { msg: str }
```

## Functions

```
f=b64encode(data:[byte]):str
f=b64urlencode(data:[byte]):str
f=b64decode(s:str):[byte]!EncodingErr
f=b64urldecode(s:str):[byte]!EncodingErr
f=hexencode(data:[byte]):str
f=hexdecode(s:str):[byte]!EncodingErr
f=urlencode(s:str):str
f=urldecode(s:str):str!EncodingErr
```

## Semantics

- `b64encode` / `b64decode` — standard Base64 (RFC 4648 section 4).
- `b64urlencode` / `b64urldecode` — URL-safe Base64 (RFC 4648 section 5).
- `hexencode` — lowercase hex output; `hexdecode` accepts mixed case.
- `urlencode` / `urldecode` — percent-encoding per RFC 3986.
- Decode functions return `EncodingErr` on malformed input.

## Dependencies

None.
