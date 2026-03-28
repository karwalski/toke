# std.test -- Test Assertions

## Overview

The `std.test` module provides assertion functions for writing tests. All functions return `bool` -- `true` (1) on pass, `false` (0) on fail. Failed assertions emit a structured diagnostic to stderr. These functions do not halt execution; the test continues after a failed assertion.

## Functions

### test.assert(cond: bool; msg: Str): bool

Passes if `cond` is `true`. On failure, emits a diagnostic containing `msg` to stderr and returns `false`.

**Example:**
```toke
test.assert(str.len("hi") == 2; "length should be 2");
test.assert(file.exists("/tmp/data.txt"); "file should exist");
```

### test.assert_eq(a: Str; b: Str; msg: Str): bool

Passes if strings `a` and `b` are equal. On failure, emits a diagnostic showing both values and `msg` to stderr and returns `false`.

**Example:**
```toke
test.assert_eq(str.upper("hello"); "HELLO"; "upper should produce HELLO");
test.assert_eq(""; ""; "empty strings are equal");
```

### test.assert_ne(a: Str; b: Str; msg: Str): bool

Passes if strings `a` and `b` are not equal. On failure, emits a diagnostic showing both values and `msg` to stderr and returns `false`.

**Example:**
```toke
test.assert_ne("foo"; "bar"; "foo and bar should differ");
test.assert_ne(""; "x"; "empty and non-empty differ");
```

## Diagnostic Output

When an assertion fails, a structured diagnostic is written to stderr. This output is consumed by the toke test runner to produce formatted failure reports.
