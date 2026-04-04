# std.json — Streaming API

## Overview

The streaming API extends `std.json` with an incremental token-pull parser and a
streaming emitter. It allows processing arbitrarily large JSON payloads without
materialising the full document in memory.

## Types

### JsonStream

An opaque parser handle returned by `json.streamparser`. Holds the input buffer,
the current read position, nesting depth, and lexer state.

| Field | Type | Meaning |
|-------|------|---------|
| buf   | @byte | Input byte slice |
| pos   | u64   | Current read offset |
| depth | u64   | Current nesting depth |
| state | u64   | Internal lexer state |

### JsonToken

A sum type representing one event from the token stream. Pull-parse by calling
`json.streamnext` in a loop until `JsonToken.End` is returned.

| Variant      | Payload | Meaning |
|--------------|---------|---------|
| ObjectStart  | void    | `{` encountered |
| ObjectEnd    | void    | `}` encountered |
| ArrayStart   | void    | `[` encountered |
| ArrayEnd     | void    | `]` encountered |
| Key          | str     | Object key string |
| Str          | str     | String value |
| U64          | u64     | Non-negative integer value |
| I64          | i64     | Negative integer value |
| F64          | f64     | Floating-point value |
| Bool         | bool    | `true` or `false` |
| Null         | void    | JSON `null` |
| End          | void    | Input exhausted |

### JsonWriter

An output buffer used with `json.streamemit`.

| Field | Type | Meaning |
|-------|------|---------|
| buf   | @byte | Output byte slice |
| pos   | u64   | Current write offset |

### JsonStreamErr

A sum type representing streaming errors.

| Variant   | Payload | Meaning |
|-----------|---------|---------|
| Truncated | str     | Input ends in the middle of a token |
| Invalid   | str     | Malformed JSON (bad escape, unexpected character, etc.) |
| Overflow  | str     | Writer buffer capacity exceeded |

## Functions

### json.streamparser(input: @byte): $jsonstream

Create a streaming parser handle over `input`. The byte slice is referenced in
place — no copy is made. The parser is single-pass and not thread-safe.

**Example:**
```toke
let data=str.bytes("[1;2;3]");
let parser=json.streamparser(data);
```

### json.streamnext(parser: $jsonstream): $jsontoken!$jsonstreamerr

Advance the parser by one token and return it. Returns `JsonToken.End` once all
input has been consumed. Returns `JsonStreamErr` on malformed input.

**Example:**
```toke
let tok=json.streamnext(parser)?;
(match tok{
  $jsontoken.U64(n){ (log.info(str.from_int(n:i64))) };
  $jsontoken.End{    break                            };
  _{}
});
```

### json.streamemit(writer: $writer; val: $json): void!$jsonstreamerr

Write the JSON representation of `val` into `writer`. Flushes incrementally so
that the writer buffer can be sized to a fixed bound and flushed by the caller.
Returns `JsonStreamErr.Overflow` if the buffer is exhausted before the value is
fully written.

**Example:**
```toke
let w=json.newwriter(4096);
json.streamemit(w;some_json)?;
let bytes=json.writerbytes(w);
file.write("/tmp/out.json";str.from_bytes(bytes)?)?;
```

### json.newwriter(capacity: u64): $writer

Allocate a `JsonWriter` with the given byte capacity.

### json.writerbytes(writer: $writer): @byte

Return a slice of the bytes written so far (from offset 0 to `writer.pos`).

## Usage Pattern — Streaming Sum

```toke
m=example;

f=sum_amounts(raw:@byte):i64!$jsonstreamerr{
  let parser=json.streamparser(raw);
  let total:i64=0;
  let want_amount:bool=false;
  loop{
    let tok=json.streamnext(parser)?;
    (match tok{
      $jsontoken.Key(k){
        if k=="amount"{ set want_amount=true };
      };
      $jsontoken.F64(v){
        if want_amount{
          set total=total+(v:i64);
          set want_amount=false;
        };
      };
      $jsontoken.End{ break };
      _{ set want_amount=false };
    });
  };
  <total
};
```
