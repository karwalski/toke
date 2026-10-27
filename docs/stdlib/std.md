---
title: std
slug: std
section: reference/stdlib
order: 63
---

**Status: Implemented** -- C runtime backing (`src/stdlib/process_glue.c`).

The root `std` module (`i=std:std;`, interface `stdlib/std.tki`) holds one function, `std.homedir`. It is an alias of [`process.homedir`](/docs/stdlib/process/) published at the top level; importing `std` pulls in `std.process` for you (138.11).

## Functions

### std.homedir(): $str

Returns the user's home directory: the value of `$HOME`, or `/tmp` when `HOME` is unset. The result is a fresh copy. It does not consult the password database (unlike [`sys.configdir`](/docs/stdlib/sys/)).

## Example

```toke
m=example;
i=io:std.io;
i=std:std;

f=main():i64{
  io.println(std.homedir());
  <0
};
```
