---
title: std.json
slug: json
section: reference/stdlib
order: 20
---

**Status: Implemented** -- C runtime backing.

The `std.json` module provides functions for parsing JSON strings into an opaque `$jsonval` value and extracting typed fields by key. It also provides object inspection, path traversal, shallow merging, and a SAX-style streaming API for large payloads.

## Types

### $jsonval

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

An opaque SAX-style streaming parser state. Created with `json.streamparser` and advanced one token at a time with `json.streamnext`. Maximum nesting depth is 256. It has no readable fields: the interface declares `buf`, `pos`, `depth` and `state`, but reading one is a compile error, E4034 (138.65).

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

### json.dec(s: $str): $jsonval!$jsonerr

Parses a JSON string into a `$jsonval` value, returning `$jsonerr.$parse` if the input is not valid JSON. Any valid JSON value (object, array, string, number, bool, or null) is accepted.

```toke
i=json:std.json;
f=example():void{
  let j=json.dec("{\"name\":\"alice\";\"age\":30}");
  let e=json.dec("not json");
  let arr=json.dec("[1;2;3]");
};
```

### json.parse(s: $str): T

Parses JSON text holding a string, a number, a bool or an array and returns that value, typed by the binding it initialises (`let t:$str=json.parse(..)`, `let n:i64=..`, `let a:@($str)=..`). String escapes are decoded. It never returns a `$jsonval` document: to read fields of an object, decode the text with `json.dec` or `json.decode`. Passing a `json.parse` result -- the call itself, or a name an un-annotated `let` bound to one -- as the document argument of `json.str`, `json.getstr`, `json.keys`, `json.has` and the other document readers is a compile error, E4031 (story 138.9).

```toke
i=json:std.json;
f=example():void{
  let name:$str=json.parse("\"alice\"");
  let n:i64=json.parse("42");
};
```

### json.str(j: $jsonval; key: $str): $str!$jsonerr

Extracts a string value from the JSON object by key, returning `$jsonerr.$missing` if the key does not exist or `$jsonerr.$type` if the value at that key is not a string.

**The key `""` on a value that is not an object** names the value itself (113.B.5), the convention `json.arr(j;"")` already uses for a top-level array. It applies to all five scalar readers (`json.str`, `json.u64`, `json.i64`, `json.f64`, `json.bool`), so a `$jsonval` that holds a scalar -- from `json.get`, `json.at` or an element of `json.arr` -- has a typed read: `json.i64(n;"")` where `n` holds `150`. On an object, `""` still names the member called `""`. Member names are matched with their JSON escapes decoded, so `"q\"k"` in the text is found by the key `q"k`.

The JSON escapes are decoded (138.12): `\n` `\t` `\r` `\b` `\f` are their control characters, `\"` `\\` `\/` their byte, and `\uXXXX` (a surrogate pair included) the code point as UTF-8; `\u0000` becomes U+FFFD, because a `$str` cannot hold a NUL. Before 138.12 the byte after each backslash was copied, so `"a\nb"` read back as `anb`. `json.keys` decodes keys the same way.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\";\"age\":30}") {$ok:v v;$err:e io.exit(1)};
  let name=json.str(j;"name");
  let bad=json.str(j;"age");
  let gone=json.str(j;"nope");
};
```

### json.u64(j: $jsonval; key: $str): u64!$jsonerr

Extracts an unsigned 64-bit integer from the JSON object by key. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not a non-negative integer: a negative number, and a number written with a fraction or an exponent (`1.5`, `1e3`), are `$type` (127.303).

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"age\":30;\"count\":0}") {$ok:v v;$err:e io.exit(1)};
  let age=json.u64(j;"age");
  let count=json.u64(j;"count");
};
```

### json.i64(j: $jsonval; key: $str): i64!$jsonerr

Extracts a signed 64-bit integer from the JSON object by key, accepting negative values that `json.u64` would reject. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not an integer: a number written with a fraction or an exponent (`1.5`, `1e3`) is `$type`, not its integer prefix (127.303).

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"temp\":-42;\"offset\":-1}") {$ok:v v;$err:e io.exit(1)};
  let temp=json.i64(j;"temp");
  let offset=json.i64(j;"offset");
};
```

### json.f64(j: $jsonval; key: $str): f64!$jsonerr

Extracts a 64-bit floating-point number from the JSON object by key. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not a number.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"pi\":3.14;\"ratio\":0.5}") {$ok:v v;$err:e io.exit(1)};
  let pi=json.f64(j;"pi");
  let ratio=json.f64(j;"ratio");
};
```

### json.bool(j: $jsonval; key: $str): bool!$jsonerr

Extracts a boolean value from the JSON object by key. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not `true` or `false`.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"active\":true;\"flag\":false}") {$ok:v v;$err:e io.exit(1)};
  let active=json.bool(j;"active");
  let flag=json.bool(j;"flag");
};
```

### json.arr(j: $jsonval; key: $str): @($jsonval)!$jsonerr

Extracts a JSON array from the object by key, returning each element as a `$jsonval` value that can be further accessed with the typed accessors. Returns `$jsonerr.$missing` if the key does not exist, or `$jsonerr.$type` if the value is not an array.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"items\":[1;2;3]}") {$ok:v v;$err:e io.exit(1)};
  let items=json.arr(j;"items");
};
```

### json.get(j: $jsonval; key: $str): $jsonval!$jsonerr

Returns the value of the member named exactly `key`, whatever its JSON type, as a `$jsonval`: read it further with the typed readers (an object by key, a scalar with the key `""`), or nest it into output with `json.setval`. Returns `$jsonerr.$type` if `j` is not an object and `$jsonerr.$missing` if it has no such member. Unlike `json.at`, the key is not split on `.`, so every name `json.keys` lists can be read back (113.B.5; before it, `json.get` was an untyped alias of `json.str`).

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"a.b\":{\"port\":8080}}") {$ok:v v;$err:e io.exit(1)};
  let ab=mt json.get(j;"a.b") {$ok:v v;$err:e j};
  let p=mt json.get(ab;"port") {$ok:v v;$err:e j};
  let port=json.i64(p;"");
};
```

### json.keys(j: $jsonval): @($str)!$jsonerr

Returns the top-level key names of a JSON object as an array of strings, in document order, with their JSON escapes decoded. Returns `$jsonerr.$type` if `j` is not an object; an empty object returns an empty array. (Before 113.B.5 both cases returned a bare null handle and the export was untyped.)

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\";\"age\":30;\"active\":true}") {$ok:v v;$err:e io.exit(1)};
  let ks=json.keys(j);
};
```

### json.entries(j: $jsonval): @($str)!$jsonerr

Returns one `"key=value"` string per member, in document order: the key with its escapes decoded, `=`, then the member's value as **JSON text** -- a string keeps its quotes (`name="alice"`), a number, bool, null, object or array is its literal text (`age=30`, `tags=["a","b"]`). Split an entry at its first `=`; a key that itself holds `=` cannot be told apart that way, so read such a member with `json.keys` and `json.get`. Returns `$jsonerr.$type` if `j` is not an object; an empty object returns an empty array. (113.B.5: a non-string value used to read as an empty string.)

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"age\":30}") {$ok:v v;$err:e io.exit(1)};
  let es=json.entries(j);
};
```

### json.has(j: $jsonval; key: $str): bool

Returns `true` if the top-level JSON object contains the given key, `false` otherwise (and `false` when `j` is not an object). This function is infallible and does not distinguish between a missing key and a null-valued key. `json.haskey(j; key)` is the same function under another name.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\"}") {$ok:v v;$err:e io.exit(1)};
  let present=json.has(j;"name");
  let absent=json.has(j;"email");
};
```

### json.len(j: $jsonval): u64!$jsonerr

Returns the number of elements in a JSON array, or the number of keys in a JSON object. Returns `$jsonerr.$type` for any other JSON type.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\";\"age\":30;\"active\":true}") {$ok:v v;$err:e io.exit(1)};
  let arr=mt json.dec("[1;2;3]") {$ok:v v;$err:e io.exit(1)};
  let alen=json.len(arr);
  let olen=json.len(j);
};
```

### json.type(j: $jsonval): $str!$jsonerr

> **Not exported.** `std.json`'s interface declares no `json.type`, so `tkc --check` rejects a call to it (E4027). The C core has `json_type` (`src/stdlib/json.c`), but no toke glue or interface entry reaches it; this section records the intended design.

Returns a string literal describing the top-level type of `j`: one of `"null"`, `"object"`, `"array"`, `"string"`, `"number"`, or `"bool"`. Useful for dynamic dispatch before using the typed accessors.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\";\"age\":30}") {$ok:v v;$err:e io.exit(1)};
  let t=json.type(j);
  let jnul=mt json.dec("null") {$ok:v v;$err:e io.exit(1)};
  let n=json.type(jnul);
  let jstr=mt json.dec("\"hi\"") {$ok:v v;$err:e io.exit(1)};
  let s=json.type(jstr);
};
```

### json.pretty(j: $jsonval): $str!$jsonerr

> **Not exported.** `std.json`'s interface declares no `json.pretty`, so `tkc --check` rejects a call to it (E4027). The C core has `json_pretty` (`src/stdlib/json.c`), but no toke glue or interface entry reaches it; this section records the intended design.

Returns a new string containing the JSON value re-serialised with 2-space indentation. Useful for logging and debugging; the output is valid JSON that can be round-tripped through `json.dec`.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\";\"age\":30}") {$ok:v v;$err:e io.exit(1)};
  let pretty=json.pretty(j);
};
```

### json.isnullkey(j: $jsonval; key: $str): i64

Returns `1` if the value at `key` is JSON `null`, or if the key does not exist at all; `0` if the key exists with any non-null value. This function is infallible. Its interface types are ABI placeholders (`"gen":"abi"`), so the result is typed `i64`, not `bool`.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\"}") {$ok:v v;$err:e io.exit(1)};
  let nilval=json.isnullkey(j;"optional");
  let present=json.isnullkey(j;"name");
};
```

### json.at(j: $jsonval; path: $str): $jsonval!$jsonerr

Traverses a dotted path through nested objects and arrays, where each segment is either an object key or a numeric array index, and returns the value there as a `$jsonval` that the typed accessors (`json.str`, `json.i64`, ...) read. Returns `$jsonerr.$missing` if any segment along the path does not exist. A key that itself contains `.` cannot be named. **Exported since 138.12** -- this is the typed nested read; before it, the only path was the untyped `json.getobj`/`json.getval`.

`json.getval(j; path)` is the same walk returning the value's raw JSON **text** as a `$str` (`""` when there is none) -- text, not a `$jsonval`: use `json.at` to read further.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"user\":{\"address\":{\"city\":\"Auckland\"}};\"results\":[{\"id\":7}]}") {$ok:v v;$err:e io.exit(1)};
  let addr=mt json.at(j;"user.address") {$ok:a a;$err:e j};
  let city=json.str(addr;"city");
  let item=json.at(j;"results.0");
};
```

### json.newobj(): $jsonval

Returns a new empty object, `{}` -- the start of a document built for output with the `json.set*` functions below.

```toke
i=json:std.json;
f=example():void{
  let o=json.newobj();
};
```

### json.set(j: $jsonval; key: $str; v: $str): $jsonval!$jsonerr

Returns a **new** object: `j` with the member `key` set to the string `v` (JSON-encoded, so quotes and control bytes are escaped). A `$jsonval` is immutable, so `j` itself is unchanged; bind the result. If `j` already has the member, its value is replaced where it stands; otherwise the member is appended, so members appear in the order they were first set. The key is always JSON-encoded, never pasted into the text. Returns `$jsonerr.$type` if `j` is not an object.

The siblings differ only in the value they write:

| Function | Writes |
|----------|--------|
| `json.setint(j: $jsonval; key: $str; v: i64)` | `v` as a JSON number |
| `json.setf64(j: $jsonval; key: $str; v: f64)` | `v` as a JSON number, in the shortest form that reads back exactly; NaN and the infinities have no JSON spelling and are `$jsonerr.$type` |
| `json.setbool(j: $jsonval; key: $str; v: bool)` | `true` or `false` |
| `json.setval(j: $jsonval; key: $str; v: $jsonval)` | `v`'s JSON text, so an object or array (from `json.newobj`, `json.dec`, `json.get`, `json.at`) nests; `$jsonerr.$parse` if `v` does not hold exactly one JSON value |

All five return `$jsonval!$jsonerr`.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let o=json.newobj();
  let o1=mt json.set(o;"name";"alice") {$ok:v v;$err:e o};
  let o2=mt json.setint(o1;"age";30) {$ok:v v;$err:e o1};
  let o3=mt json.setbool(o2;"admin";false) {$ok:v v;$err:e o2};
  let tags=mt json.dec("[\"a\"]") {$ok:v v;$err:e o};
  let o4=mt json.setval(o3;"tags";tags) {$ok:v v;$err:e o3};
  io.println(json.serialize(o4));
};
```

### json.setint(j: $jsonval; key: $str; v: i64): $jsonval!$jsonerr

`json.set` for an integer value; see `json.set`.

### json.setf64(j: $jsonval; key: $str; v: f64): $jsonval!$jsonerr

`json.set` for a floating-point value; NaN and the infinities are `$jsonerr.$type`. See `json.set`.

### json.setbool(j: $jsonval; key: $str; v: bool): $jsonval!$jsonerr

`json.set` for a boolean value; see `json.set`.

### json.setval(j: $jsonval; key: $str; v: $jsonval): $jsonval!$jsonerr

`json.set` for any JSON value, including a nested object or array; `$jsonerr.$parse` if `v` does not hold exactly one JSON value. See `json.set`.

### json.serialize(j: $jsonval): $str

Returns the document's JSON text -- what a document built with `json.newobj` and `json.set*` hands to `io.println`, `file.write` or an HTTP response. Infallible.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let o=json.newobj();
  let o1=mt json.set(o;"status";"ok") {$ok:v v;$err:e o};
  io.println(json.serialize(o1));
};
```

### json.index(j: $jsonval; i: u64): $jsonval!$jsonerr

> **Not exported.** `std.json`'s interface declares no `json.index`, so `tkc --check` rejects a call to it (E4027). The C core has `json_index` (`src/stdlib/json.c`), but no toke glue or interface entry reaches it; this section records the intended design.

Returns the element at position `i` in a JSON array. Returns `$jsonerr.$type` if `j` is not an array, and `$jsonerr.$missing` if `i` is out of bounds.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let arr=mt json.dec("[10;20;30]") {$ok:v v;$err:e io.exit(1)};
  let first=json.index(arr;0);
  let oob=json.index(arr;5);
};
```

### json.merge(j1: $jsonval; j2: $jsonval): $jsonval!$jsonerr

> **Not exported.** `std.json`'s interface declares no `json.merge`, so `tkc --check` rejects a call to it (E4027). The C core has `json_merge` (`src/stdlib/json.c`), but no toke glue or interface entry reaches it; this section records the intended design.

Produces a new JSON object that is a shallow merge of `j1` and `j2`, with keys from `j2` overriding those in `j1`. Returns `$jsonerr.$type` if either argument is not a JSON object.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let base=mt json.dec("{\"a\":1;\"b\":2}") {$ok:v v;$err:e io.exit(1)};
  let override=mt json.dec("{\"b\":99;\"c\":3}") {$ok:v v;$err:e io.exit(1)};
  let merged=json.merge(base;override);
};
```

### json.frompairs(keys: @($str); values: @($str)): $jsonval

> **Not exported.** `std.json`'s interface declares no `json.frompairs`, so `tkc --check` rejects a call to it (E4027). The C core has `json_from_pairs` (`src/stdlib/json.c`), but no toke glue or interface entry reaches it; this section records the intended design.

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

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Creates a SAX-style streaming parser over a byte buffer without copying it. The returned `$jsonstream` must be advanced with `json.streamnext` until a `$jsontoken.$end` or error is returned.

```toke
i=json:std.json;
i=file:std.file;
f=example():void{
  let rawbuf=mt file.read("/data/big.json") {$ok:v v;$err:e @()};
  let stream=json.streamparser(rawbuf);
};
```

### json.streamnext(s: $jsonstream): $jsontoken!$jsonstreamerr

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Advances the streaming parser by one token and returns it. Returns `$jsonstreamerr.$truncated` if the input ends mid-value, `$jsonstreamerr.$invalid` for malformed input, or `$jsonstreamerr.$overflow` if nesting exceeds 256 levels.

```toke
i=json:std.json;
i=file:std.file;
f=example():void{
  let buf=mt file.read("/data/big.json") {$ok:v v;$err:e @()};
  let stream=json.streamparser(buf);
  let tok=json.streamnext(stream);
  let key=json.streamnext(stream);
  let val=json.streamnext(stream);
};
```

### json.newwriter(initial_cap: u64): $jsonwriter

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Allocates a new `$jsonwriter` with the given initial byte capacity. The writer grows automatically as tokens are emitted; passing `0` uses a default capacity.

```toke
i=json:std.json;
f=example():void{
  let w=json.newwriter(512);
};
```

### json.streamemit(w: $jsonwriter; j: $jsonval): void!$jsonstreamerr

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Emits the complete JSON value `j` into the writer `w` as a stream of bytes. Returns `$jsonstreamerr.$invalid` if `j` contains malformed JSON.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\"}") {$ok:v v;$err:e io.exit(1)};
  let w=json.newwriter(256);
  mt json.streamemit(w;j) {$ok:x x;$err:e io.exit(1)};
};
```

### json.writerbytes(w: $jsonwriter): @($byte)

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Returns the bytes accumulated in the writer so far as a byte slice. The writer retains ownership; call after all `json.streamemit` calls are complete to obtain the final output.

```toke
i=json:std.json;
i=io:std.io;
f=example():void{
  let j=mt json.dec("{\"name\":\"alice\"}") {$ok:v v;$err:e io.exit(1)};
  let w=json.newwriter(256);
  mt json.streamemit(w;j) {$ok:x x;$err:e io.exit(1)};
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
  let ok=mt json.dec(raw) {$ok:v 1;$err:e 0};
  if(ok==1){
    io.println("parsed ok");
  }el{};
  <0;
};
```

### Reading nested values and building a reply

`json.at` walks a dotted path to a value; the key `""` reads a scalar `$jsonval` (113.B.5). The reply is built member by member and serialised once.

```toke
m=example;
i=json:std.json;
i=io:std.io;

f=main():i64{
  let req=mt json.dec("{\"user\":{\"name\":\"alice\"}}") {$ok:v v;$err:e json.newobj()};
  let n=mt json.at(req;"user.name") {$ok:v v;$err:e req};
  let name=mt json.str(n;"") {$ok:v v;$err:e "?"};
  let out=json.newobj();
  let o1=mt json.set(out;"greeting";"hello \(name)") {$ok:v v;$err:e out};
  let o2=mt json.setint(o1;"code";200) {$ok:v v;$err:e o1};
  io.println(json.serialize(o2));
  <0;
};
```

The functions marked **Not exported** or quarantined above are not reachable from toke; everything else on this page is.

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
