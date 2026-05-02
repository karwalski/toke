---
title: std.json
slug: json
section: reference/stdlib
order: 20
---

**Status: Implemented** -- C runtime backing.

The `std.json` module provides functions for parsing JSON strings into an opaque `$json` value and extracting typed fields by key. It also provides object inspection, path traversal, shallow merging, and a SAX-style streaming API for large payloads.

## Types

### $json

An opaque wrapper around a raw JSON string. Obtained by calling `json.dec` on a valid JSON input. Field access is performed via the typed accessor functions (`json.str`, `json.u64`, etc.).

| Field | Type | Meaning |
|-------|------|---------|
| raw | $str | The underlying JSON string |

### $jsonwriter

A streaming JSON writer that accumulates output into an internal byte buffer. Created with `json.newwriter`; bytes are retrieved with `json.writerbytes`.

| Field | Type | Meaning |
|-------|------|---------|
| buf | @($byte) | The accumulated output buffer |
| pos | u64 | Current write position |

### $jsonstream

An opaque SAX-style streaming parser state. Created with `json.streamparser` and advanced one token at a time with `json.streamnext`. Maximum nesting depth is 256.

| Field | Type | Meaning |
|-------|------|---------|
| buf | @($byte) | The input byte buffer |
| pos | u64 | Current read position |
| depth | u64 | Current nesting depth |
| state | u64 | Internal parser state machine value |

### $jsontoken

A sum type representing a single SAX-style event emitted by the streaming parser.

| Variant | Payload | Meaning |
|---------|---------|---------|
| $objectstart | void | Opening `{` encountered |
| $objectend | void | Closing `}` encountered |
| $arraystart | void | Opening `[` encountered |
| $arrayend | void | Closing `]` encountered |
| $key | $str | An object key string |
| $str | $str | A string value |
| $u64 | u64 | An unsigned integer value |
| $i64 | i64 | A signed integer value |
| $f64 | f64 | A floating-point value |
| $bool | bool | A boolean value |
| $null | void | A null value |
| $end | void | End of input |

## Functions

### json.enc(v: $str): $str

Encodes a plain string as a JSON string literal, surrounding it with double quotes and escaping any special characters. The result is always valid JSON.

```toke
i=json:std.json;
f=example():void{
  let j=json.enc("hello \"world\"");
  let plain=json.enc("simple");
};
```

### json.dec(s: $str): $json!$jsonerr

Parses a JSON string into a `$json` value, returning `$jsonerr.$parse` if the input is not valid JSON. Any valid JSON value (object, array, string, number, bool, or null) is accepted.

```toke
i=json:std.json;
f=example():void{
  let j=json.dec("{\"name\":\"alice\";\"age\":30}");
  let e=json.dec("not json");
  let arr=json.dec("[1;2;3]");
};
```

### json.str(j: $json; key: $str): $str!$jsonerr

Extracts a string value from the JSON object by key, returning `$jsonerr.$missing` if the key does not exist or `$jsonerr.$type` if the value at that key is not a string.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\";\"age\":30}")|{$ok:v v;$err:e io.exit(1)};
  let name=json.str(j;"name");
  let bad=json.str(j;"age");
  let gone=json.str(j;"nope");
};
```

### json.u64(j: $json; key: $str): u64!$jsonerr

Extracts an unsigned 64-bit integer from the JSON object by key. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not a non-negative integer.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"age\":30;\"count\":0}")|{$ok:v v;$err:e io.exit(1)};
  let age=json.u64(j;"age");
  let count=json.u64(j;"count");
};
```

### json.i64(j: $json; key: $str): i64!$jsonerr

Extracts a signed 64-bit integer from the JSON object by key, accepting negative values that `json.u64` would reject. Returns `$jsonerr.$missing` or `$jsonerr.$type` on failure.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"temp\":-42;\"offset\":-1}")|{$ok:v v;$err:e io.exit(1)};
  let temp=json.i64(j;"temp");
  let offset=json.i64(j;"offset");
};
```

### json.f64(j: $json; key: $str): f64!$jsonerr

Extracts a 64-bit floating-point number from the JSON object by key. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not a number.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"pi\":3.14;\"ratio\":0.5}")|{$ok:v v;$err:e io.exit(1)};
  let pi=json.f64(j;"pi");
  let ratio=json.f64(j;"ratio");
};
```

### json.bool(j: $json; key: $str): bool!$jsonerr

Extracts a boolean value from the JSON object by key. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not `true` or `false`.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"active\":true;\"flag\":false}")|{$ok:v v;$err:e io.exit(1)};
  let active=json.bool(j;"active");
  let flag=json.bool(j;"flag");
};
```

### json.arr(j: $json; key: $str): @($json)!$jsonerr

Extracts a JSON array from the object by key, returning each element as a `$json` value that can be further accessed with the typed accessors. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not an array.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"items\":[1;2;3]}")|{$ok:v v;$err:e io.exit(1)};
  let items=json.arr(j;"items");
};
```

### json.keys(j: $json): @($str)!$jsonerr

Returns the top-level key names of a JSON object as an array of strings. Returns `$jsonerr.$type` if `j` is not an object; an empty object returns an empty array.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\";\"age\":30;\"active\":true}")|{$ok:v v;$err:e io.exit(1)};
  let ks=json.keys(j);
};
```

### json.has(j: $json; key: $str): bool

Returns `true` if the top-level JSON object contains the given key, `false` otherwise. This function is infallible and does not distinguish between a missing key and a null-valued key.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\"}")|{$ok:v v;$err:e io.exit(1)};
  let present=json.has(j;"name");
  let absent=json.has(j;"email");
};
```

### json.len(j: $json): u64!$jsonerr

Returns the number of elements in a JSON array, or the number of keys in a JSON object. Returns `$jsonerr.$type` for any other JSON type.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\";\"age\":30;\"active\":true}")|{$ok:v v;$err:e io.exit(1)};
  let arr=json.dec("[1;2;3]")|{$ok:v v;$err:e io.exit(1)};
  let alen=json.len(arr);
  let olen=json.len(j);
};
```

### json.type(j: $json): $str!$jsonerr

Returns a string literal describing the top-level type of `j`: one of `"null"`, `"object"`, `"array"`, `"string"`, `"number"`, or `"bool"`. Useful for dynamic dispatch before using the typed accessors.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\";\"age\":30}")|{$ok:v v;$err:e io.exit(1)};
  let t=json.type(j);
  let jnul=json.dec("null")|{$ok:v v;$err:e io.exit(1)};
  let n=json.type(jnul);
  let jstr=json.dec("\"hi\"")|{$ok:v v;$err:e io.exit(1)};
  let s=json.type(jstr);
};
```

### json.pretty(j: $json): $str!$jsonerr

Returns a new string containing the JSON value re-serialised with 2-space indentation. Useful for logging and debugging; the output is valid JSON that can be round-tripped through `json.dec`.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\";\"age\":30}")|{$ok:v v;$err:e io.exit(1)};
  let pretty=json.pretty(j);
};
```

### json.isnull(j: $json; key: $str): bool

Returns `true` if the value at `key` is JSON `null`, or if the key does not exist at all. Returns `false` if the key exists with any non-null value. This function is infallible.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\"}")|{$ok:v v;$err:e io.exit(1)};
  let nilval=json.isnull(j;"optional");
  let present=json.isnull(j;"name");
};
```

### json.at(j: $json; path: $str): $json!$jsonerr

Traverses a dotted path through nested objects and arrays, where each segment is either an object key or a numeric array index. Returns `$jsonerr.$missing` if any segment along the path does not exist.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"user\":{\"address\":{\"city\":\"Auckland\"}}}")|{$ok:v v;$err:e io.exit(1)};
  let city=json.at(j;"user.address.city");
  let item=json.at(j;"results.0.id");
};
```

### json.index(j: $json; i: u64): $json!$jsonerr

Returns the element at position `i` in a JSON array. Returns `$jsonerr.$type` if `j` is not an array, and `$jsonerr.$missing` if `i` is out of bounds.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let arr=json.dec("[10;20;30]")|{$ok:v v;$err:e io.exit(1)};
  let first=json.index(arr;0);
  let oob=json.index(arr;5);
};
```

### json.merge(j1: $json; j2: $json): $json!$jsonerr

Produces a new JSON object that is a shallow merge of `j1` and `j2`, with keys from `j2` overriding those in `j1`. Returns `$jsonerr.$type` if either argument is not a JSON object.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let base=json.dec("{\"a\":1;\"b\":2}")|{$ok:v v;$err:e io.exit(1)};
  let override=json.dec("{\"b\":99;\"c\":3}")|{$ok:v v;$err:e io.exit(1)};
  let merged=json.merge(base;override);
};
```

### json.frompairs(keys: @($str); values: @($str)): $json

Constructs a JSON object from parallel arrays of key and value strings. Each value string is JSON-encoded via `json.enc` before insertion. This function is infallible; mismatched array lengths produce an object truncated to the shorter length.

```toke
i=json:std.json;
f=example():void{
  let ks=@("host";"port");
  let vs=@("localhost";"8080");
  let j=json.frompairs(ks;vs);
};
```

### json.streamparser(buf: @($byte)): $jsonstream

Creates a SAX-style streaming parser over a byte buffer without copying it. The returned `$jsonstream` must be advanced with `json.streamnext` until a `$jsontoken.$end` or error is returned.

```toke
i=json:std.json;
i=file:std.file;
f=example():void{
  let rawbuf=file.read("/data/big.json")|{$ok:v v;$err:e @()};
  let stream=json.streamparser(rawbuf);
};
```

### json.streamnext(s: $jsonstream): $jsontoken!$jsonstreamerr

Advances the streaming parser by one token and returns it. Returns `$jsonstreamerr.$truncated` if the input ends mid-value, `$jsonstreamerr.$invalid` for malformed input, or `$jsonstreamerr.$overflow` if nesting exceeds 256 levels.

```toke
i=json:std.json;
i=file:std.file;
f=example():void{
  let buf=file.read("/data/big.json")|{$ok:v v;$err:e @()};
  let stream=json.streamparser(buf);
  let tok=json.streamnext(stream);
  let key=json.streamnext(stream);
  let val=json.streamnext(stream);
};
```

### json.newwriter(initial_cap: u64): $jsonwriter

Allocates a new `$jsonwriter` with the given initial byte capacity. The writer grows automatically as tokens are emitted; passing `0` uses a default capacity.

```toke
i=json:std.json;
f=example():void{
  let w=json.newwriter(512);
};
```

### json.streamemit(w: $jsonwriter; j: $json): void!$jsonstreamerr

Emits the complete JSON value `j` into the writer `w` as a stream of bytes. Returns `$jsonstreamerr.$invalid` if `j` contains malformed JSON.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\"}")|{$ok:v v;$err:e io.exit(1)};
  let w=json.newwriter(256);
  json.streamemit(w;j)|{$ok:x x;$err:e io.exit(1)};
};
```

### json.writerbytes(w: $jsonwriter): @($byte)

Returns the bytes accumulated in the writer so far as a byte slice. The writer retains ownership; call after all `json.streamemit` calls are complete to obtain the final output.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=json.dec("{\"name\":\"alice\"}")|{$ok:v v;$err:e io.exit(1)};
  let w=json.newwriter(256);
  json.streamemit(w;j)|{$ok:x x;$err:e io.exit(1)};
  let bytes=json.writerbytes(w);
};
```

## Usage Examples

### Encoding and round-trip decode

The following example uses the currently implemented `json.enc` and `json.dec` functions to encode a string value and verify it round-trips through the parser.

```toke
m=example;
i=json:std.json;
i=io:std.io;

f=main():i64{
  let encoded=json.enc("hello \"world\"");
  io.println(encoded);

  let raw="{\"status\":\"ok\"}";
  let ok=json.dec(raw)|{$ok:v 1;$err:e 0};
  if(ok=1){
    io.println("parsed ok");
  }el{};
  <0;
};
```

> **Note:** Higher-level accessor functions (`json.str`, `json.u64`, `json.arr`, `json.at`, `json.from_pairs`, streaming parser, etc.) are documented above and are planned for a future release. Only `json.enc` and `json.dec` are implemented in the current compiler.

## Error Types

### $jsonerr

A sum type representing failures from the high-level accessor and parse functions.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $parse | $str | The input string is not valid JSON |
| $type | $str | The value exists but is not the expected type |
| $missing | $str | The requested key or path segment does not exist |

### $jsonstreamerr

A sum type representing failures from the SAX-style streaming parser and writer.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $truncated | $str | The input ended before a complete JSON value was read |
| $invalid | $str | Malformed JSON was encountered at the current position |
| $overflow | $str | Nesting depth exceeded the 256-level limit |

## See Also

- [std.str](/docs/stdlib/str) — string manipulation used with JSON string values
- [std.file](/docs/stdlib/file) — reading JSON payloads from disk before parsing
- [std.http](/docs/stdlib/http) — HTTP client and server that typically exchange JSON bodies
