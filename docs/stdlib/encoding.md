---
title: std.encoding
slug: encoding
section: reference/stdlib
order: 12
---

**Status: Implemented** -- C runtime backing.

The `std.encoding` module provides functions for encoding and decoding binary data to and from common text representations: Base64, Base64-URL, hexadecimal, and percent-encoded URLs.

## Types

### $encodingerr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable error description |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `encoding.b64encode` | `data: @($byte)` | `$str` | Encode bytes to standard Base64 |
| `encoding.b64urlencode` | `data: @($byte)` | `$str` | Encode bytes to URL-safe Base64 |
| `encoding.b64decode` | `s: $str` | `@($byte)!$encodingerr` | Decode standard Base64 to bytes |
| `encoding.b64urldecode` | `s: $str` | `@($byte)!$encodingerr` | Decode URL-safe Base64 to bytes |
| `encoding.hexencode` | `data: @($byte)` | `$str` | Encode bytes to lowercase hex string |
| `encoding.hexdecode` | `s: $str` | `@($byte)!$encodingerr` | Decode hex string to bytes |
| `encoding.urlencode` | `s: $str` | `$str` | Percent-encode a string for use in URLs |
| `encoding.urldecode` | `s: $str` | `$str!$encodingerr` | Decode a percent-encoded string |

## Usage

```toke
m=app;
i=enc:std.encoding;
i=crypto:std.crypto;
i=str:std.str;
i=log:std.log;

f=main():i64{
  let key=crypto.sha256(str.tobytes("secret"));
  let hex=enc.hexencode(key);
  log.info(hex;@());

  let param=enc.urlencode("key=value&foo=bar baz");
  log.info(param;@());
  <0
};
```

## Combined Example

```toke
m=app;
i=enc:std.encoding;
i=crypto:std.crypto;
i=log:std.log;

f=main():i64{
  let raw=crypto.randombytes(16);

  let b64=enc.b64encode(raw);
  let b64url=enc.b64urlencode(raw);
  let hex=enc.hexencode(raw);

  log.info(b64;@());
  log.info(b64url;@());
  log.info(hex;@());

  let decoded=enc.b64decode(b64)|{$ok:v v;$err:e @()};
  let rehex=enc.hexencode(decoded);
  log.info(rehex;@());

  let query=enc.urlencode("search=hello world&page=1");
  let back=enc.urldecode(query)|{$ok:s s;$err:e ""};
  log.info(back;@());

  <0
};
```

## Dependencies

None.
