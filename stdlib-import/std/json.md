# std.json -- JSON Encoding and Decoding

## Overview

The `std.json` module provides functions for parsing JSON strings into an opaque `Json` value and extracting typed fields by key. It also provides a simple encoding function for producing JSON string literals.

## Types

### Json

An opaque wrapper around a raw JSON string. Obtained by calling `json.dec` on a valid JSON input. Field access is performed via the typed accessor functions (`json.str`, `json.u64`, etc.).

| Field | Type | Meaning |
|-------|------|---------|
| raw | Str | The underlying JSON string |

## Functions

### json.enc(v: Str): Str

Encodes a string value as a JSON string literal (with surrounding double quotes and escaping).

**Example:**
```toke
let j = json.enc("hello");  (* j = "\"hello\"" *)
```

### json.dec(s: Str): Json!JsonErr

Parses a JSON string into a `Json` value. Returns `JsonErr.Parse` if the input is not valid JSON.

**Example:**
```toke
let j = json.dec("{\"name\":\"alice\";\"age\":30}");
(* j = ok(Json{...}) *)

let e = json.dec("not json");
(* e = err(JsonErr.Parse{...}) *)
```

### json.str(j: Json; key: Str): Str!JsonErr

Extracts a string value from the JSON object by key. Returns `JsonErr.Missing` if the key does not exist, or `JsonErr.Type` if the value is not a string.

**Example:**
```toke
let name = json.str(j; "name");  (* name = ok("alice") *)
```

### json.u64(j: Json; key: Str): u64!JsonErr

Extracts an unsigned 64-bit integer from the JSON object by key. Returns `JsonErr.Missing` if the key does not exist, or `JsonErr.Type` if the value is not a number.

**Example:**
```toke
let age = json.u64(j; "age");  (* age = ok(30) *)
```

### json.i64(j: Json; key: Str): i64!JsonErr

Extracts a signed 64-bit integer from the JSON object by key. Returns `JsonErr.Missing` if the key does not exist, or `JsonErr.Type` if the value is not a number.

**Example:**
```toke
let temp = json.i64(j; "temp");  (* temp = ok(-42) *)
```

### json.f64(j: Json; key: Str): f64!JsonErr

Extracts a 64-bit float from the JSON object by key. Returns `JsonErr.Missing` if the key does not exist, or `JsonErr.Type` if the value is not a number.

**Example:**
```toke
let pi = json.f64(j; "pi");  (* pi = ok(3.14) *)
```

### json.bool(j: Json; key: Str): bool!JsonErr

Extracts a boolean value from the JSON object by key. Returns `JsonErr.Missing` if the key does not exist, or `JsonErr.Type` if the value is not a boolean.

**Example:**
```toke
let flag = json.bool(j; "flag");  (* flag = ok(true) *)
```

### json.arr(j: Json; key: Str): [Json]!JsonErr

Extracts a JSON array from the object by key, returning each element as a `Json` value. Returns `JsonErr.Missing` if the key does not exist, or `JsonErr.Type` if the value is not an array.

**Example:**
```toke
let items = json.arr(j; "items");  (* items = ok([Json; Json; Json]) *)
```

## Error Types

### JsonErr

A sum type representing JSON operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| Parse | Str | The input string is not valid JSON |
| Type | Str | The value exists but is not the expected type |
| Missing | Str | The requested key does not exist in the object |
