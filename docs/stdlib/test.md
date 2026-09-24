---
title: std.test
slug: test
section: reference/stdlib
order: 36
---

**Status: Implemented** -- C runtime backing.

The `std.test` module provides assertion functions for writing tests. All functions return `bool` -- `true` (1) on pass, `false` (0) on fail. Failed assertions emit a structured diagnostic to stderr. These functions do not halt execution; the test continues after a failed assertion.

## Functions

### test.assert(cond: bool; msg: $str): bool

Passes if `cond` is `true`. On failure, emits a diagnostic containing `msg` to stderr and returns `false`.

```toke
m=tests;
i=test:std.test;
i=str:std.str;
i=file:std.file;
f=main():i64{
  let r1=test.assert(str.len("hi")==2;"length should be 2");
  let r2=test.assert(file.exists("/tmp/data.txt");"file should exist");
  < 0
};
```

### test.asserteq(a: $str; b: $str; msg: $str): bool

Passes if strings `a` and `b` are equal. On failure, emits a diagnostic showing both values and `msg` to stderr and returns `false`.

```toke
m=tests;
i=test:std.test;
i=str:std.str;
f=main():i64{
  let r1=test.asserteq(str.upper("hello");"HELLO";"upper should produce HELLO");
  let r2=test.asserteq("";""; "empty strings are equal");
  < 0
};
```

### test.assertne(a: $str; b: $str; msg: $str): bool

Passes if strings `a` and `b` are not equal. On failure, emits a diagnostic showing both values and `msg` to stderr and returns `false`.

```toke
m=tests;
i=test:std.test;
f=main():i64{
  let r1=test.assertne("foo";"bar";"foo and bar should differ");
  let r2=test.assertne("";"x";"empty and non-empty differ");
  < 0
};
```

### test.eq(a: i64; b: i64): bool

Passes if `a` and `b` are equal. On failure, emits a diagnostic showing both values to stderr, marks the current test failed, and returns `false`.

When either argument is a `$str` (and neither is a number or `bool`), the two strings are compared **by value** -- `test.eq(str.upper("hello"); "HELLO")` passes. Before 127.68 a string pair compared the two addresses and failed for equal strings. When the compiler cannot tell that an argument is a string, the integer comparison is used; write `test.eqstr` to say so explicitly.

```toke
m=tests;
i=test:std.test;
i=str:std.str;
f=main():i64{
  let r1=test.eq(2+2;4);
  let r2=test.eq(str.upper("hello");"HELLO");
  < 0
};
```

### test.eqstr(a: $str; b: $str): bool

Passes if strings `a` and `b` are equal by value. On failure, emits a diagnostic showing both strings to stderr, marks the current test failed, and returns `false`. Use it where the string type of an argument is not evident from the expression; `test.asserteq` is the same comparison with a caller-supplied message.

```toke
m=tests;
i=test:std.test;
i=str:std.str;
f=main():i64{
  let r1=test.eqstr(str.lower("ABC");"abc");
  < 0
};
```

## Usage Examples

```toke
m=tests;
i=test:std.test;
i=str:std.str;

f=main():i64{
  let r1=test.asserteq(str.trim("  hi  ");"hi";"trim removes whitespace");
  let r2=test.assert(str.contains("foobar";"oba");"contains finds substring");
  let parts=str.split("a:b:c";":");
  let r3=test.asserteq(str.fromint(parts.len as i64);"3";"split produces 3 parts");
  let iserr=mt str.toint("not a number") {$ok:n false;$err:e true};
  let r4=test.assert(iserr;"toint rejects non-numeric input");
  < 0
};
```

## Diagnostic Output

When an assertion fails, a structured diagnostic is written to stderr. This output is consumed by the toke test runner to produce formatted failure reports.
