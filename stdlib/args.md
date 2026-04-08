# std.args -- Command-Line Arguments

## Overview

The `std.args` module provides read-only access to the command-line arguments passed to the toke program at startup. Arguments are captured by the runtime before the toke entry point runs and do not change for the lifetime of the process.

Key points:

- `argv[0]` (index 0) is always the program name as supplied by the OS.
- Arguments are immutable after startup; there is no `args.set` function.
- The toke entry point remains `f=main():i64`. Command-line arguments are not passed as parameters to `main`; they are accessed by importing `std.args` and calling the functions below.
- `args.count()` always returns at least 1 (the program name).

## Functions

### args.count(): u64

Returns the total number of command-line arguments, including `argv[0]` (the program name).

**Example:**
```toke
import std.args;

let n = args.count();  (* n >= 1 *)
```

### args.get(n: u64): str!ArgsErr

Returns the argument at index `n`. Index 0 is the program name. Returns `ArgsErr` if `n` is out of bounds.

**Example:**
```toke
import std.args;

let name = args.get(0);  (* ok: program name *)
let flag = args.get(1);  (* ok if argc > 1, else err *)

match flag {
  ok(s)  => (* use s *),
  err(e) => (* e.msg describes the problem *)
}
```

### args.all(): [str]

Returns all arguments as an array of strings. The first element is the program name.

**Example:**
```toke
import std.args;

let all = args.all();
let n   = args.count();

(* iterate over user-supplied args, skipping argv[0] *)
let i = 1;
while i < n {
  let a = args.get(i);
  (* process a *)
  i = i + 1;
}
```

**Bulk pattern using args.all:**
```toke
import std.args;

let argv = args.all();
(* argv[0] is the program name; argv[1..] are user flags *)
```

## Error Types

### ArgsErr

Returned by `args.get` when the requested index is out of bounds.

| Field | Type | Meaning |
|-------|------|---------|
| msg   | str  | Human-readable description of the error |
