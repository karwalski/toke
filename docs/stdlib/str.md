---
title: std.str
slug: str
section: reference/stdlib
order: 33
---

**Status: Implemented** -- C runtime backing.

The `std.str` module provides string manipulation functions for UTF-8 encoded strings. All strings in toke are immutable, null-terminated UTF-8 byte sequences. Functions that transform strings return new allocations.

## Functions

### str.len(s: $str): u64

Returns the byte length of the string `s`.

```toke
m=test;
i=str:std.str;
f=example():void{
  let n = str.len("hello");
  let z = str.len("");
};
```

### str.concat(a: $str; b: $str): $str

Returns a new string formed by appending `b` to `a`.

```toke
m=test;
i=str:std.str;
f=example():void{
  let s = str.concat("foo"; "bar");
  let t = str.concat(""; "x");
};
```

### str.slice(s: $str; start: u64; end: u64): $str!$sliceerr

Returns the substring of `s` from byte index `start` (inclusive) to `end` (exclusive). Returns `$sliceerr` if either index exceeds the string length or if `start > end`.

```toke
m=test;
i=str:std.str;
f=example():void{
  let r = str.slice("hello"; 1; 3);
  let e = str.slice("hello"; 3; 10);
};
```

### str.fromint(n: i64): $str

Converts a signed 64-bit integer to its decimal string representation.

```toke
m=test;
i=str:std.str;
f=example():void{
  let s = str.fromint(42);
  let t = str.fromint(-7);
};
```

### str.fromfloat(n: f64): $str

Converts a 64-bit float to its string representation.

```toke
m=test;
i=str:std.str;
f=example():void{
  let s = str.fromfloat(3.14);
};
```

### str.toint(s: $str): i64!$parseerr

Parses a decimal integer from the string `s`. Returns `$parseerr` if the string is not a valid integer or is empty.

```toke
m=test;
i=str:std.str;
f=example():void{
  let n = str.toint("123");
  let e = str.toint("abc");
};
```

### str.tofloat(s: $str): f64!$parseerr

Parses a floating-point number from the string `s`. Returns `$parseerr` if the string is not a valid number.

```toke
m=test;
i=str:std.str;
f=example():void{
  let f = str.tofloat("2.5");
  let e = str.tofloat("xyz");
};
```

### str.contains(s: $str; sub: $str): bool

Returns `true` if `s` contains the substring `sub`, `false` otherwise.

```toke
m=test;
i=str:std.str;
f=example():void{
  let y = str.contains("foobar"; "oba");
  let n = str.contains("foobar"; "xyz");
};
```

### str.indexof(s: $str; sub: $str): i64

Returns the byte offset of the first occurrence of `sub` in `s`, or `-1` if `sub` does not occur.

```toke
m=test;
i=str:std.str;
f=example():void{
  let a = str.indexof("a,b,c"; ",");
  let n = str.indexof("abc"; "x");
};
```

### str.indexfrom(s: $str; sub: $str; start: i64): i64

`str.indexof` with a start offset: returns the byte offset (counted from the start of `s`, not from `start`) of the first occurrence of `sub` at or after byte `start`, or `-1`. A negative `start` searches from 0; a `start` past the end of `s` returns `-1`. Use it to walk a string without slicing off the tail before each search.

The bounds check reads the `start` bytes before the search point (toke strings store no length), so each call costs O(`start`) reads — but no allocation and no copy, where slicing the tail allocated and copied the rest of the string on every call.

```toke
m=test;
i=io:std.io;
i=str:std.str;
f=example():void{
  let t = "a,b,,c";
  let n = mut.0;
  let p = mut.str.indexfrom(t; ","; 0);
  lp(let k = 0; p >= 0; k = k + 1){
    n = n + 1;
    p = str.indexfrom(t; ","; p + 1)
  };
  io.println("commas=\(n)");
};
```

### str.split(s: $str; sep: $str): @($str)

Splits the string `s` on each occurrence of the separator `sep` and returns an array of substrings.

```toke
m=test;
i=str:std.str;
f=example():void{
  let parts = str.split("a:b:c"; ":");
};
```

### str.trim(s: $str): $str

Returns a new string with leading and trailing whitespace removed.

```toke
m=test;
i=str:std.str;
f=example():void{
  let t = str.trim("  hi  ");
};
```

### str.upper(s: $str): $str

Returns a new string with all ASCII characters converted to uppercase.

```toke
m=test;
i=str:std.str;
f=example():void{
  let u = str.upper("hello");
};
```

### str.lower(s: $str): $str

Returns a new string with all ASCII characters converted to lowercase.

```toke
m=test;
i=str:std.str;
f=example():void{
  let l = str.lower("WORLD");
};
```

### str.bytes(s: $str): @($byte)

Returns the raw UTF-8 byte array of the string.

```toke
m=test;
i=str:std.str;
f=example():void{
  let b = str.bytes("abc");
};
```

### str.frombytes(b: @($byte)): $str!$encodingerr

Converts a byte array to a string. Returns `$encodingerr` if the bytes are not valid UTF-8.

```toke
m=test;
i=str:std.str;
f=example():void{
  let s = str.frombytes(@(104; 105));
};
```

## Usage Examples

```toke
m=test;
i=str:std.str;
f=example(rawinput:$str):void{
  let input=str.trim(rawinput);
  let val=mt str.toint(input) {$ok:n n;$err:e 0};
  let first=mt str.slice("alice";0;1) {$ok:s s;$err:e ""};
  let name=str.upper(first);
  let rest=mt str.slice("alice";1;5) {$ok:s s;$err:e ""};
  let greeting=str.concat("Hello ";str.concat(name;rest));
  let fields=str.split("name:age:city";":");
};
```

## Combined Example

Split a delimited string on `":"`, uppercase the first word, then rejoin.

```toke
m=example;
i=str:std.str;
i=io:std.io;

f=main():i64{
  let input="apple:banana:cherry";
  let parts=str.split(input;":");
  let first=str.upper(parts.get(0));
  let result=str.join(":";@(first;parts.get(1);parts.get(2)));
  io.println(result);
  <0;
};
```

## Error Types

### $sliceerr

Returned by `str.slice` when indices are out of bounds.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable description of the error |

### $parseerr

Returned by `str.toint` and `str.tofloat` when the input string cannot be parsed as the target numeric type.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable description of the parse failure |

### $encodingerr

Returned by `str.frombytes` when the byte array is not valid UTF-8.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable description of the encoding error |
