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
  let r1=test.assert(str.len("hi")=2;"length should be 2");
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
