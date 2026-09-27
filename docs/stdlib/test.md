---
title: std.test
slug: test
section: reference/stdlib
order: 36
---

**Status: Implemented** -- C runtime backing.

The `std.test` module provides assertion functions for writing tests. The assertions return `bool` -- `true` (1) on pass, `false` (0) on fail (`test.ok` and `test.fail` return the same as `i64`). Failed assertions emit a structured diagnostic to stderr. These functions do not halt execution; the test continues after a failed assertion, so every failure in a run is reported -- and **a run with any failed assertion exits non-zero**, however `main` ends: with `main`'s own status when that is non-zero, otherwise 1. See [Exit status](#exit-status).

## Functions

### test.assert(cond: bool; msg: $str): bool

Passes if `cond` is `true`. On failure, emits a diagnostic containing `msg` to stderr and returns `false`.

```toke
m=tests;
i=test:std.test;
i=str:std.str;
i=file:std.file;
f=main():i64{
  let path=str.concat(file.tempdir();"/toke-test-example.txt");
  let w=mt file.write(path;"data") {$ok:b b; $err:e false};
  let r1=test.assert(str.len("hi")==2;"length should be 2");
  let r2=test.assert(file.exists(path);"file should exist after it is written");
  let d=mt file.delete(path) {$ok:b b; $err:e false};
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

### test.ok(v: i64): i64

Passes if `v` is non-zero -- a `bool` condition or a count. On failure, emits `DIAGNOSTIC: kind=assertion_failed msg="test.ok: value is 0 (false)"` to stderr, marks the current test failed, and returns `0`; on success it returns `1`. (Before 127.231 it could not fail: a zero value printed and marked nothing.)

```toke
m=tests;
i=test:std.test;
i=str:std.str;
f=main():i64{
  let r1=test.ok(str.contains("foobar";"oba"));
  let r2=test.ok(str.len("abc"));
  < 0
};
```

### test.fail(msg: $str): i64

Fails unconditionally: prints `FAIL: msg` to stderr, marks the current test failed, and returns `0`. Use it on a branch that must not be reached.

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

## Exit status

A test program fails through its **exit status**, not only through its output (story 127.231). The first failed assertion -- `test.assert`, `test.asserteq`, `test.assertne`, `test.eq`, `test.eqstr`, `test.ok` or `test.fail` -- records the failure; the run continues, so every later failure is still reported; and when the process exits, it prints

```text
DIAGNOSTIC: kind=exit_status status=1 failures=2 first="std.test assertion failed"
```

and exits with status **1** -- or with `main`'s own status, when `main` returned a non-zero one (the line then says `status=3`, say). A run with no failed assertion exits with whatever `main` returns. Stated so nobody has to rediscover them:

- A program that returns `3` after a failed assertion exits `3` (story 135.29). One that returns `0` exits `1`. A status whose low 8 bits are zero (`256`) would read as success, so it exits `1` too.
- `os.exit(0)` after a failed assertion also exits `1`, with the same line. `os.exit` with a non-zero status exits with that status.
- A process killed by a signal is unaffected.
- The same rule applies to a server that does not start (`std.http`, see [Starting, failing and the port](/docs/stdlib/http/#starting-failing-and-the-port)).
