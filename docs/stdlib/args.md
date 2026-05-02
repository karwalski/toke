---
title: std.args -- Command-Line Arguments
slug: args
section: reference/stdlib
order: 2
---

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
m=example;
i=args:std.args;

f=main():i64{
  let n = args.count();  -- n >= 1
  < 0
};
```

### args.get(n: u64): $str!$argserr

Returns the argument at index `n`. Index 0 is the program name. Returns `$argserr` if `n` is out of bounds.

**Example:**
```toke
m=example;
i=args:std.args;

f=main():i64{
  let name = args.get(0)|{$ok:s s;$err:e ""};
  let flag = args.get(1)|{$ok:s s;$err:e ""};
  < 0
};
```

### args.all(): @($str)

Returns all arguments as an array of strings. The first element is the program name.

**Example:**
```toke
m=example;
i=args:std.args;

f=main():i64{
  let all = args.all();
  let n   = args.count();

  -- iterate over user-supplied args, skipping argv[0]
  lp(let i=1;i<n;i=i+1){
    let a = args.get(i)|{$ok:s s;$err:e ""};
  };
  < 0
};
```

**Bulk pattern using args.all:**
```toke
m=example;
i=args:std.args;

f=main():i64{
  let argv = args.all();
  -- argv.get(0) is the program name; argv.get(1..) are user flags
  < 0
};
```

## Error Types

### $argserr

Returned by `args.get` when the requested index is out of bounds.

| Field | Type | Meaning |
|-------|------|---------|
| msg   | $str | Human-readable description of the error |
