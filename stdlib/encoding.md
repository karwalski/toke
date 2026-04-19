# std.encoding -- Binary and Text Encoding

## Overview

The `std.encoding` module provides functions for encoding and decoding between binary data and text representations. It covers Base64 (standard and URL-safe), hexadecimal, and percent-encoding (URL encoding). Encode functions are infallible; decode functions return `!EncodingErr` when the input contains invalid characters or malformed sequences.

## Types

### EncodingErr

Returned when a decode function encounters invalid input.

| Field | Type | Meaning |
|-------|------|---------|
| msg | str | Human-readable description of the error |

## Functions

### encoding.b64encode(data: [byte]): str

Encodes raw bytes to a standard Base64 string (RFC 4648, alphabet A-Z a-z 0-9 +/ with = padding).

**Example:**
```toke
i=enc:std.encoding;

let s=enc.b64encode(@(72; 101; 108; 108; 111));
(* s = "SGVsbG8=" *)

let empty=enc.b64encode(@());
(* empty = "" *)
```

### encoding.b64urlencode(data: [byte]): str

Encodes raw bytes to a URL-safe Base64 string (RFC 4648 section 5, alphabet A-Z a-z 0-9 -_ with = padding). Suitable for use in URLs and filenames.

**Example:**
```toke
i=enc:std.encoding;

let s=enc.b64urlencode(@(251; 239));
(* s = "--8=" — uses - and _ instead of + and / *)
```

### encoding.b64decode(s: str): [byte]!EncodingErr

Decodes a standard Base64 string back to raw bytes. Returns `EncodingErr` if the string contains characters outside the Base64 alphabet or has invalid padding.

**Example:**
```toke
i=enc:std.encoding;

let result=enc.b64decode("SGVsbG8=");
(match result{
  ok(bytes){
    (* bytes = @(72; 101; 108; 108; 111) — "Hello" *)
  };
  err(e){
    (log.error(e.msg));
  };
});

let bad=enc.b64decode("not!valid");
(* bad = err(EncodingErr{msg: "invalid character at position 3"}) *)
```

### encoding.b64urldecode(s: str): [byte]!EncodingErr

Decodes a URL-safe Base64 string back to raw bytes. Returns `EncodingErr` if the input is malformed.

**Example:**
```toke
i=enc:std.encoding;

let result=enc.b64urldecode("--8=");
(match result{
  ok(bytes){
    (* bytes = @(251; 239) *)
  };
  err(e){
    (log.error(e.msg));
  };
});
```

### encoding.hexencode(data: [byte]): str

Encodes raw bytes to a lowercase hexadecimal string. Each input byte produces exactly two hex characters.

**Example:**
```toke
i=enc:std.encoding;

let s=enc.hexencode(@(0xDE; 0xAD; 0xBE; 0xEF));
(* s = "deadbeef" *)

let s2=enc.hexencode(@(0; 255));
(* s2 = "00ff" *)
```

### encoding.hexdecode(s: str): [byte]!EncodingErr

Decodes a hexadecimal string to raw bytes. The input must have an even number of characters, each in the range 0-9 a-f A-F. Returns `EncodingErr` on invalid input.

**Example:**
```toke
i=enc:std.encoding;

let result=enc.hexdecode("deadbeef");
(match result{
  ok(bytes){
    (* bytes = @(0xDE; 0xAD; 0xBE; 0xEF) *)
  };
  err(e){
    (log.error(e.msg));
  };
});

let bad=enc.hexdecode("abc");
(* bad = err(EncodingErr{msg: "odd number of hex characters"}) *)
```

### encoding.urlencode(s: str): str

Percent-encodes a string for safe inclusion in a URL component (RFC 3986). Unreserved characters (A-Z a-z 0-9 - _ . ~) are left unchanged; all others are replaced with %XX sequences.

**Example:**
```toke
i=enc:std.encoding;

let s=enc.urlencode("hello world");
(* s = "hello%20world" *)

let s2=enc.urlencode("key=value&foo=bar");
(* s2 = "key%3Dvalue%26foo%3Dbar" *)

let safe=enc.urlencode("already-safe_123");
(* safe = "already-safe_123" — no change *)
```

### encoding.urldecode(s: str): str!EncodingErr

Decodes a percent-encoded string. Each %XX sequence is replaced with the corresponding byte. Returns `EncodingErr` if a percent sign is not followed by exactly two hex digits or the decoded bytes are not valid UTF-8.

**Example:**
```toke
i=enc:std.encoding;

let result=enc.urldecode("hello%20world");
(match result{
  ok(s){
    (* s = "hello world" *)
  };
  err(e){
    (log.error(e.msg));
  };
});

let bad=enc.urldecode("bad%ZZinput");
(* bad = err(EncodingErr{msg: "invalid percent-encoding at position 3"}) *)
```

## Round-Trip Examples

Encode and decode are inverses. A round-trip always recovers the original data.

```toke
i=enc:std.encoding;

(* Base64 round-trip *)
let original=@(1; 2; 3; 4; 5);
let encoded=enc.b64encode(original);
let decoded=enc.b64decode(encoded);
(* decoded = ok(@(1; 2; 3; 4; 5)) *)

(* Hex round-trip *)
let hex_str=enc.hexencode(original);
let hex_back=enc.hexdecode(hex_str);
(* hex_back = ok(@(1; 2; 3; 4; 5)) *)

(* URL encoding round-trip *)
let url_str=enc.urlencode("price=10&tax=1.5");
let url_back=enc.urldecode(url_str);
(* url_back = ok("price=10&tax=1.5") *)
```

## Notes

- Base64 standard and URL-safe variants differ only in two alphabet characters (`+/` vs `-_`). Mixing them in decode produces `EncodingErr`.
- `hexencode` always produces lowercase output. `hexdecode` accepts both upper and lowercase input.
- `urlencode` encodes every reserved character. To encode only the query-string value portion, encode the value before concatenating with `=` and `&`.
