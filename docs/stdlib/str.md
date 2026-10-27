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

The bounds check is O(1) when `s` carries its length — a string from interpolation, `s.concat`, `slice`, the builder's `build`, `split`, `join`, `replace`, `file.read` or `io.readln` does (runtime-abi §3.1). For any other string it reads the `start` bytes before the search point, so the call costs O(`start`) reads. Either way there is no allocation and no copy, where slicing the tail allocated and copied the rest of the string on every call.

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

### str.compare(a: $str; b: $str): i64

Byte-wise ordering, as `strcmp`: negative if `a` sorts before `b`, `0` if they are equal, positive after (the value is `-1`, `0` or `1`). Before 138.11 it returned `0` for equal and `1` for anything else, so it could not order.

### str.isdigit(s: $str): bool

`true` if `s` is non-empty and every byte is `0`-`9`. It takes a string: `str.isdigit(str.charat(s;i))` passes a byte **code**, which the checker does not yet reject; since 138.11 the glue reads a code below 65536 as the byte it is instead of dereferencing it (it used to crash, SIGSEGV).

### str.padleft(s: $str; width: i64; pad: i64): $str

Pads `s` on the left to `width` bytes with the byte whose code is `pad` (`0` means a space): `str.padleft("7";3;48)` is `"007"`. A string passed as `pad` -- which the checker does not yet reject -- pads with its first byte since 138.11; it used to pad with the low byte of its address. `str.padright` is the same on the right.

### str.padright(s: $str; width: i64; pad: i64): $str

See `str.padleft`.

### str.nowunix(): $str

The current time in whole seconds since the Unix epoch, as decimal text (`"1790510572"`). It is a `$str`: for arithmetic use `time.now()` (milliseconds, `i64`).

### str.tof32(s: $str): f64!$parseerr

An alias of `str.tofloat` (so are `str.tof64` and `str.parsefloat`); 138.11 gave all three `str.tofloat`'s type. Before, they were untyped and `str.tof32(12345)` dereferenced address 12345 (rc 139).

## Regular expressions

Five functions match a pattern, and a sixth, `str.checkre`, checks one and returns the error as a value. They share one engine (`src/stdlib/tk_regex.h`, stories 137.9 and 121.35) and one language, the same on every platform. Before 137.9 they called POSIX `regcomp`/`regexec`: a pattern `regcomp` rejected — `\S` on glibc, `*?`, `(?i)`, a malformed `[` — returned *no match* with no error, and on macOS `\s`, `\d`, `\w` and `\b` compiled as the literal letters `s`, `d`, `w` and `b`.

### str.checkre(pattern: $str): $str!$regexerr

`pattern` unchanged if it is in the pattern language below, else a `$regexerr` — `{msg: $str; pos: i64}`, the reason and the byte offset into the pattern of the construct that failed (`-1` when the failure is not at a byte, such as running out of memory). It is decided by the same compiler the five matching calls use, so a pattern `str.checkre` accepts never stops them with `RT008`. It is never `RT008` itself, and a literal argument is not `E4075`: returning the error is its purpose. Use it for any pattern that comes from outside the program (story 121.45):

```toke
m=example;
i=str:std.str;
f=search(text:$str;userpattern:$str):$str{
  <mt str.checkre(userpattern){
    $ok:p str.fromint(str.countre(text;p));
    $err:e str.concat("bad pattern: ";e.msg)
  }
};
```

`str.checkre` does not cover the step budget: a valid pattern can still stop `str.countre`, `str.findall` or `str.replacere` with `RT009` (see **Cost** below).

### str.containsre(s: $str; pattern: $str): i64

`1` if `pattern` matches anywhere in `s`, else `0`.

### str.matches(s: $str; pattern: $str): i64

`1` if `pattern` matches the **whole** of `s`, else `0` — `str.matches("x123";"[0-9]+")` is `0`; use `str.containsre` to search. The whole-string rule is applied by the matcher; before 137.9 it was applied by pasting `^(`…`)$` around the pattern text, undocumented, so an unbalanced pattern such as `a)(b` became valid and a search-style pattern silently never matched.

### str.countre(s: $str; pattern: $str): i64

The number of non-overlapping matches, scanning left to right.

### str.findall(s: $str; pattern: $str): @($str)

Every non-overlapping match, in order.

### str.replacere(s: $str; pattern: $str; replacement: $str): $str

`s` with every non-overlapping match replaced by `replacement`, which is taken literally (no `\1` group references).

### The pattern language

| Syntax | Meaning |
|---|---|
| `a`, `\.`, `\*`, `\{` … | a literal byte; `\` before any punctuation (or a space) makes it literal |
| `.` | any byte |
| `^` `$` | start / end of the whole string (not of a line) |
| `[abc]` `[^abc]` `[a-z]` | a bracket expression; `]` first and `-` first or last are literal |
| `[:alpha:]` `[:digit:]` `[:alnum:]` `[:upper:]` `[:lower:]` `[:space:]` `[:blank:]` `[:punct:]` `[:print:]` `[:graph:]` `[:cntrl:]` `[:xdigit:]` | POSIX classes, inside a bracket expression |
| `\s` `\S` `\d` `\D` `\w` `\W` | ASCII space, digit and word (`[A-Za-z0-9_]`) classes and their complements; also inside brackets |
| `\b` `\B` | word boundary / not a word boundary |
| `\n` `\t` `\r` `\f` `\v` | control characters (inside brackets too) |
| `(…)` `(?:…)` | grouping (there are no capture groups) |
| `a\|b` | alternation |
| `*` `+` `?` `{n}` `{n,}` `{n,m}` | greedy repetition, `n` and `m` at most 255 |
| `(?i)` | at the very start only: ASCII case-insensitive matching |

Anything else is **rejected**, never silently read as something else: lazy quantifiers (`*?` `+?` `??` `{n,m}?`), backreferences (`\1`), lookaround and every other `(?…)` form, a letter escape not in the table, a quantifier with nothing to repeat or following another quantifier, an unbalanced `(` `)` or `[`, a `{` that does not start a bound (write `\{`), an empty alternative (`a|`, `()`), and a pattern whose repeats expand past 2000 states (`(a{1,255}){1,255}`).

A **literal** pattern is checked when the program is compiled — `toke --check` reports [E4075](/docs/reference/errors/#e4075) at the offending byte. So is a literal bound by an immutable `let` that is the name's only binder in the function, when the call follows it in its block (`let p="a(b"; s.containsre(x;p)` is E4075 at the `(`):

<!-- skip-check -->
```text
error[E4075]: invalid regex pattern for str.replacere: lazy quantifiers (*? +? ?? {n,m}?) are not supported: …
```

A pattern **computed** at run time is checked when it is used; an invalid one stops the program with `RT008: invalid regex pattern in str.<fn>: <reason> (at byte N of "<pattern>")` and exit status 1. To get the error as a value instead, pass the pattern through `str.checkre` first and use the pattern it returns.

**Matching rule.** Matching is byte-wise. A search finds the leftmost match and, from that start, the longest one (POSIX leftmost-longest): `str.findall("ab";"a|ab")` is `["ab"]`. After a match the scan resumes at its end; after an empty match it moves one byte on, and an empty match directly after the previous match is skipped, so `str.replacere("abc";"x*";"-")` is `"-a-b-c-"`.

**Cost.** Matching simulates the pattern's automaton without backtracking, so there is no exponential case: one search reads each byte once, at a cost proportional to the pattern's size after repeats are expanded (at most 2000 states). Each call has a step budget of eight full passes over the subject plus 2^26 steps. A single search never exhausts it; the scan-resume loop of `str.countre`, `str.findall` and `str.replacere` can — a pattern such as `a|a*b` re-reads the rest of the subject after every match, which is quadratic — and the program then stops with `RT009: regex step budget exceeded in str.<fn>` and exit status 1 rather than run on. Measured on an Apple M2 Pro (2026-09-29): about 450 million steps a second; that pattern over 16 000 bytes stops after about 0.15 s, and after about 0.2 s over 160 000 (POSIX `regexec` took 2.4 s over 16 000 and quadrupled with each doubling); a near-limit pattern of 1533 states searches about 4.5 µs per subject byte.

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
