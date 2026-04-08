# std.str -- String Operations

## Overview

The `std.str` module provides string manipulation functions for UTF-8 encoded strings. All strings in toke are immutable, null-terminated UTF-8 byte sequences. Functions that transform strings return new allocations.

## Functions

### str.len(s: Str): u64

Returns the byte length of the string `s`.

**Example:**
```toke
let n = str.len("hello");  (* n = 5 *)
let z = str.len("");        (* z = 0 *)
```

### str.concat(a: Str; b: Str): Str

Returns a new string formed by appending `b` to `a`.

**Example:**
```toke
let s = str.concat("foo"; "bar");  (* s = "foobar" *)
let t = str.concat(""; "x");       (* t = "x" *)
```

### str.slice(s: Str; start: u64; end: u64): Str!SliceErr

Returns the substring of `s` from byte index `start` (inclusive) to `end` (exclusive). Returns `SliceErr` if either index exceeds the string length or if `start > end`.

**Example:**
```toke
let r = str.slice("hello"; 1; 3);  (* r = ok("el") *)
let e = str.slice("hello"; 3; 10); (* e = err(SliceErr{...}) *)
```

### str.from_int(n: i64): Str

Converts a signed 64-bit integer to its decimal string representation.

**Example:**
```toke
let s = str.from_int(42);   (* s = "42" *)
let t = str.from_int(-7);   (* t = "-7" *)
```

### str.from_float(n: f64): Str

Converts a 64-bit float to its string representation.

**Example:**
```toke
let s = str.from_float(3.14);  (* s = "3.14" *)
```

### str.to_int(s: Str): i64!ParseErr

Parses a decimal integer from the string `s`. Returns `ParseErr` if the string is not a valid integer or is empty.

**Example:**
```toke
let n = str.to_int("123");  (* n = ok(123) *)
let e = str.to_int("abc");  (* e = err(ParseErr{...}) *)
```

### str.to_float(s: Str): f64!ParseErr

Parses a floating-point number from the string `s`. Returns `ParseErr` if the string is not a valid number.

**Example:**
```toke
let f = str.to_float("2.5");  (* f = ok(2.5) *)
let e = str.to_float("xyz");  (* e = err(ParseErr{...}) *)
```

### str.contains(s: Str; sub: Str): bool

Returns `true` if `s` contains the substring `sub`, `false` otherwise.

**Example:**
```toke
let y = str.contains("foobar"; "oba");  (* y = true *)
let n = str.contains("foobar"; "xyz");  (* n = false *)
```

### str.split(s: Str; sep: Str): [Str]

Splits the string `s` on each occurrence of the separator `sep` and returns an array of substrings.

**Example:**
```toke
let parts = str.split("a,b,c"; ",");
(* parts = ["a"; "b"; "c"] *)
```

### str.trim(s: Str): Str

Returns a new string with leading and trailing whitespace removed.

**Example:**
```toke
let t = str.trim("  hi  ");  (* t = "hi" *)
```

### str.upper(s: Str): Str

Returns a new string with all ASCII characters converted to uppercase.

**Example:**
```toke
let u = str.upper("hello");  (* u = "HELLO" *)
```

### str.lower(s: Str): Str

Returns a new string with all ASCII characters converted to lowercase.

**Example:**
```toke
let l = str.lower("WORLD");  (* l = "world" *)
```

### str.bytes(s: Str): [Byte]

Returns the raw UTF-8 byte array of the string.

**Example:**
```toke
let b = str.bytes("abc");  (* b = [97; 98; 99] *)
```

### str.from_bytes(b: [Byte]): Str!EncodingErr

Converts a byte array to a string. Returns `EncodingErr` if the bytes are not valid UTF-8.

**Example:**
```toke
let s = str.from_bytes([104; 105]);  (* s = ok("hi") *)
```

## Error Types

### SliceErr

Returned by `str.slice` when indices are out of bounds.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable description of the error |

### ParseErr

Returned by `str.to_int` and `str.to_float` when the input string cannot be parsed as the target numeric type.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable description of the parse failure |

### EncodingErr

Returned by `str.from_bytes` when the byte array is not valid UTF-8.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable description of the encoding error |

---

## Story 55.4 — Search, Transform, and String Builder

### str.startswith(s: Str; prefix: Str): bool

Returns `true` if `s` begins with `prefix`, `false` otherwise.

**Example:**
```toke
let y = str.startswith("foobar"; "foo");  (* y = true *)
let n = str.startswith("foobar"; "bar");  (* n = false *)
```

### str.endswith(s: Str; suffix: Str): bool

Returns `true` if `s` ends with `suffix`, `false` otherwise.

**Example:**
```toke
let y = str.endswith("foobar"; "bar");  (* y = true *)
let n = str.endswith("foobar"; "foo");  (* n = false *)
```

### str.replace(s: Str; old: Str; new: Str): Str

Returns a new string with every occurrence of `old` replaced by `new`. If `old` is not found the original string is returned as a new copy.

**Example:**
```toke
let r = str.replace("aabbaa"; "aa"; "x");  (* r = "xbbx" *)
let u = str.replace("hello"; "z"; "y");    (* u = "hello" *)
```

### str.indexof(s: Str; sub: Str): i64

Returns the byte offset of the first occurrence of `sub` in `s`, or `-1` if not found.

**Example:**
```toke
let i = str.indexof("foobar"; "bar");  (* i = 3 *)
let m = str.indexof("foobar"; "xyz");  (* m = -1 *)
```

### str.repeat(s: Str; n: u64): Str

Returns a new string consisting of `s` repeated `n` times. Returns an empty string when `n` is zero.

**Example:**
```toke
let r = str.repeat("ab"; 3);  (* r = "ababab" *)
let z = str.repeat("x"; 0);   (* z = "" *)
```

### str.join(sep: Str; parts: [Str]): Str

Joins the elements of `parts` into a single string, placing `sep` between each adjacent pair. Returns an empty string when `parts` is empty.

**Example:**
```toke
let s = str.join(", "; ["a"; "b"; "c"]);  (* s = "a, b, c" *)
let e = str.join("-"; []);                (* e = "" *)
```

---

## String Builder — `$strbuf`

The `StrBuf` type is a mutable, growable buffer for building strings incrementally. Use it instead of repeated `str.concat` calls when assembling many pieces; `str.concat` is O(n) per call, making a loop O(n²) overall. `StrBuf` amortises allocation with doubling, giving O(n) total.

### str.buf(): StrBuf

Allocates a new, empty string builder with an initial internal capacity of 256 bytes.

```toke
let b = str.buf();
```

### str.add(b: StrBuf; s: Str): void

Appends the string `s` to the builder `b`, growing the buffer if needed.

```toke
str.add(b; "hello");
str.add(b; ", ");
str.add(b; "world");
```

### str.addbyte(b: StrBuf; c: byte): void

Appends a single byte `c` to the builder `b`. Useful when emitting control characters or building binary-safe payloads one byte at a time.

```toke
str.addbyte(b; 10);  (* append newline 0x0A *)
```

### str.done(b: StrBuf): Str

Finalises the builder: shrinks the internal allocation to fit, returns the completed heap string, and resets `b` to an empty/invalid state. The returned string is owned by the caller.

```toke
let result = str.done(b);
```

### Full example — building an HTML snippet

```toke
let b    = str.buf();
let tags = ["h1"; "p"; "footer"];
let i    = 0u64;
loop {
    if i >= str.len(str.from_int(cast(i64; i))) { break; }
    str.add(b; "<");
    str.add(b; tags[i]);
    str.add(b; ">");
    i = i + 1u64;
    if i >= 3u64 { break; }
};
let html = str.done(b);
(* html = "<h1><p><footer>" *)
```

A more direct example using a known list:

```toke
let b = str.buf();
str.add(b; "<div class=\"");
str.add(b; "container");
str.add(b; "\">");
str.add(b; "Hello, toke!");
str.add(b; "</div>");
let html = str.done(b);
(* html = "<div class=\"container\">Hello, toke!</div>" *)
```
