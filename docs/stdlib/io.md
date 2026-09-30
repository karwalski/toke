---
title: std.io
slug: io
section: reference/stdlib
order: 53
---

**Status: Implemented** -- C runtime backing (`src/stdlib/io_glue.c`).

The `std.io` module is console I/O: text to standard output and standard
error, and lines from standard input. Every write takes a `$str`, which is
NUL-terminated, so it stops at the first zero byte; for raw descriptors, see
[std.os](/docs/stdlib/os/).

| Stream | No newline added | Newline added |
|--------|------------------|---------------|
| stdout | `io.print` | `io.println` |
| stderr | `io.eprint` | `io.eprintln` |

stdout is buffered by the C library and stderr is not, so text written to the
two streams can reach a terminal in a different order from the calls. A
program that interleaves them and cares about the order writes to one.

> **Interface note.** Every export except `io.eprint` is tagged `"gen":"abi"`
> in `io.tki`: its parameter types are ABI placeholders (`i64`) and only its
> arity is established. The parameters below are what the glue reads.

## Functions

### io.print(s: $str): i64

Writes `s` to stdout with no newline. Returns `0`.

### io.println(s: $str): i64

Writes `s` and a newline to stdout. Returns `0`.

```toke
m=example;
i=io:std.io;

f=main():i64{
  io.print("a");
  io.println("b");
  <0
};
```

### io.eprint(s: $str): void

Writes `s` to stderr with no newline (121.70). The stderr twin of `io.print`:
use it to build one diagnostic line from parts, or for a prompt or progress
display that must not mix with the program's output on stdout. An empty `s`
writes nothing.

```toke
m=example;
i=io:std.io;

f=main():i64{
  io.eprint("warning: ");
  io.eprint("disk nearly full");
  io.eprintln("");
  <0
};
```

### io.eprintln(s: $str): i64

Writes `s` and a newline to stderr. Returns `0`.

### io.printf(fmt: $str; val: $str): i64

Writes `val` formatted by the C `printf` format `fmt` to stdout. `val` is
passed as a C string, so the one conversion `fmt` may contain is `%s`.
Returns `0`. Prefer string interpolation (`"\(x)"`) and `io.print`.

### io.readln(): $str

Reads one line from stdin, without its newline. At end of input it returns
`""` -- which a blank line also returns -- so call `io.eof` to tell them
apart. Lines longer than 4095 bytes are returned in pieces.

### io.eof(): i64

`1` if the most recent `io.readln` reached end of input rather than reading a
line (possibly blank), else `0`.

```toke
m=example;
i=io:std.io;

f=main():i64{
  lp(let n=0;n<1000000;n=n+1){
    let l=io.readln();
    if(io.eof()==1){br};
    io.println(l)
  };
  <0
};
```

## See Also

- [std.os](/docs/stdlib/os/) -- `os.write` to a raw descriptor, with a byte count and an error
- [std.log](/docs/stdlib/log/) -- structured logging
