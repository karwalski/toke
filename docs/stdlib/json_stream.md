---
title: std.json — Streaming API
slug: json_stream
section: reference/stdlib
order: 21
---

## Overview

The streaming API extends `std.json` with an incremental token-pull parser and a
streaming emitter. It allows processing arbitrarily large JSON payloads without
materialising the full document in memory.

## Types

### $jsonstream

An opaque parser handle returned by `json.streamparser`. Holds the input buffer,
the current read position, nesting depth, and lexer state.

| Field | Type | Meaning |
|-------|------|---------|
| buf   | @($byte) | Input byte slice |
| pos   | u64   | Current read offset |
| depth | u64   | Current nesting depth |
| state | u64   | Internal lexer state |

### $jsontoken

A sum type representing one event from the token stream. Pull-parse by calling
`json.streamnext` in a loop until `$jsontoken.$end` is returned.

| Variant      | Payload | Meaning |
|--------------|---------|---------|
| $objectstart  | void    | `{` encountered |
| $objectend    | void    | `}` encountered |
| $arraystart   | void    | `[` encountered |
| $arrayend     | void    | `]` encountered |
| $key          | $str    | Object key string |
| $str          | $str    | String value |
| $u64          | u64     | Non-negative integer value |
| $i64          | i64     | Negative integer value |
| $f64          | f64     | Floating-point value |
| $bool         | bool    | `true` or `false` |
| $null         | void    | JSON `null` |
| $end          | void    | Input exhausted |

### $jsonwriter

An output buffer used with `json.streamemit`.

| Field | Type | Meaning |
|-------|------|---------|
| buf   | @($byte) | Output byte slice |
| pos   | u64   | Current write offset |

### $jsonstreamerr

A sum type representing streaming errors.

| Variant   | Payload | Meaning |
|-----------|---------|---------|
| $truncated | $str    | Input ends in the middle of a token |
| $invalid   | $str    | Malformed JSON (bad escape, unexpected character, etc.) |
| $overflow  | $str    | Writer buffer capacity exceeded |

## Functions

### json.streamparser(input: @($byte)): $jsonstream

Create a streaming parser handle over `input`. The byte slice is referenced in
place — no copy is made. The parser is single-pass and not thread-safe.

**Example:**
```toke
let data=str.bytes("[1;2;3]");
let parser=json.streamparser(data);
```

### json.streamnext(parser: $jsonstream): $jsontoken!$jsonstreamerr


Advance the parser by one token and return it. Returns `$jsontoken.$end` once all
input has been consumed. Returns `$jsonstreamerr` on malformed input.

**Example:**
```toke
let tok=json.streamnext(parser)?;
(match tok{
  $jsontoken.$u64(n){ (log.info(str.fromint(n:i64))) };
  $jsontoken.$end{    break                            };
  _{}
});
```

### json.streamemit(writer: $jsonwriter; val: $json): void!$jsonstreamerr

Write the JSON representation of `val` into `writer`. Flushes incrementally so
that the writer buffer can be sized to a fixed bound and flushed by the caller.
Returns `$jsonstreamerr.$overflow` if the buffer is exhausted before the value is
fully written.

**Example:**
```toke
let w=json.newwriter(4096);
json.streamemit(w;some_json)?;
let bytes=json.writerbytes(w);
file.write("/tmp/out.json";str.frombytes(bytes)?)?;
```

### json.newwriter(capacity: u64): $writer

Allocate a `$jsonwriter` with the given byte capacity.

### json.writerbytes(writer: $writer): @byte

Return a slice of the bytes written so far (from offset 0 to `writer.pos`).

## Usage Pattern — Streaming Sum

```text
m=example;

f=sumamounts(raw:@(u8)):i64!$jsonstreamerr{
  let parser=json.streamparser(raw);
  let total:i64=0;
  let wantamount:bool=false;
  loop{
    let tok=json.streamnext(parser)?;
    (match tok{
      $jsontoken.key(k){
        if k=="amount"{ set wantamount=true };
      };
      $jsontoken.f64(v){
        if wantamount{
          set total=total+(v:i64);
          set wantamount=false;
        };
      };
      $jsontoken.end{ break };
      _{ set wantamount=false };
    });
  };
  <total
};
```
