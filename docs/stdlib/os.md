---
title: std.os
slug: os
section: reference/stdlib
order: 54
---

**Status: Implemented** -- C runtime backing (`src/stdlib/os.c`, `src/stdlib/os_glue.c`).

The `std.os` module is a thin bridge to POSIX: file descriptors, paths,
process, environment and `errno`. Each call returns what the underlying
syscall returns -- usually `-1` on failure, with the reason in `os.errno()` --
**except `os.write`**, which writes every byte or fails through `$err`.

Reach for [std.file](/docs/stdlib/file/), [std.io](/docs/stdlib/io/) and
[std.env](/docs/stdlib/env/) first: they carry typed errors on every call.
`std.os` is for what they do not cover -- a raw descriptor, a specific
`open` flag, `lseek`.

Strings are NUL-terminated `$str`s, so `os.read` and `os.write` stop at the
first zero byte: for binary data use `file.readbytes` / `file.writebytes`.

**Capabilities.** Under `--cap-enforce`, `os.open` needs `fs.read` for a read
mode and `fs.write` for any write, create, truncate or append flag (both for
`O_RDWR`); `os.stat` and `os.access` need `fs.read`; `os.unlink`,
`os.rename`, `os.mkdir` and `os.rmdir` need `fs.write`; `os.setenv` needs
`env.write`. The calls on an already-open descriptor are not gated.

## Types

### $oserr

The error `os.write` raises: a record, not a sum.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | `os.write: <errno text>: wrote N of M bytes` -- how far it got |
| code | i64 | the errno (`9` is `EBADF`, `32` is `EPIPE`); `os.errno()` in the `$err` arm agrees |

## Functions

### os.open(path: $str; flags: i64; mode: i64): i64

Opens `path` with open(2) and returns the descriptor, or `-1`. `flags` is a
sum of the flag constants below; `mode` is the permission bits for a created
file (`420` is octal 644). A symlink in the final component is **not**
followed (`O_NOFOLLOW` is always added, AMB-07): opening one fails with
`ELOOP`.

### os.close(fd: i64): i64

close(2): `0`, or `-1`.

### os.read(fd: i64; count: i64): $str

Reads up to `count` bytes from `fd` and returns them as a new string the
caller owns. End of input and an error both return `""`; check `os.errno()`
to tell them apart.

### os.write(fd: i64; data: $str): i64!$oserr

Writes **every** byte of `data` to `fd` and returns the count (the length of
`data`; `0` for `""`, which is a success). A write the kernel takes only part
of is continued, and one interrupted by a signal (`EINTR`) is retried. A
failure is the `$err` arm: `e.code` is the errno and `e.msg` says how many
bytes reached the descriptor before it. On a non-blocking descriptor,
`EAGAIN` is a failure, not a retry.

**Changed in 121.70.** `os.write` used to return `i64`: one write(2), whose
partial count was returned as if it were the whole, and `0` on error -- the
same value a write of `""` returns. `let n=o.write(fd;s)` still compiles
and binds the count; match on the call to see failures.

```toke
m=example;
i=io:std.io;
i=o:std.os;

f=main():i64{
  <mt o.write(o.stderrfd();"partial line, no newline") {
    $ok:n 0;
    $err:e 1
  }
};
```

### os.lseek(fd: i64; offset: i64; whence: i64): i64

lseek(2): the new offset, or `-1`. `whence` is `0` (from the start), `1`
(from the current offset) or `2` (from the end).

### os.stat(path: $str): i64

The size in bytes of what `path` names (stat(2), which follows symlinks), or
`-1`. For a checked size that refuses directories and symlinks, use
`file.size`.

### os.unlink(path: $str): i64

unlink(2): `0`, or `-1`.

### os.rename(from: $str; to: $str): i64

rename(2): `0`, or `-1`.

### os.mkdir(path: $str; mode: i64): i64

mkdir(2) of one directory with permission bits `mode` (`493` is octal 755):
`0`, or `-1`.

### os.rmdir(path: $str): i64

rmdir(2) of an empty directory: `0`, or `-1`.

### os.access(path: $str; mode: i64): i64

access(2): `0` if the process may access `path` in `mode` (`0` exists, `4`
read, `2` write, `1` execute, summed), else `-1`.

### os.getcwd(): $str

The working directory, as a new string the caller owns; `""` if getcwd(3)
fails.

### os.getpid(): i64

The process id.

### os.exit(status: i64): i64

Ends the process at once with `status` (\_exit(2): `atexit` handlers and
buffered stdout are **not** flushed -- write with `os.write` or flush first).
`os.exit(0)` after a recorded failure, such as a failed test assertion,
exits `1` instead. `process.exit` follows the same rule (127.532). Does not
return.

### os.getenv(name: $str): $str

The value of environment variable `name`, or `""` if it is unset. The string
is a copy the program owns: a later `os.setenv` of the same name does not
change or invalidate it.

### os.setenv(name: $str; value: $str): i64

setenv(3), overwriting: `0`, or `-1`.

### os.errno(): i64

The C `errno` left by the most recent failing call.

### os.strerror(code: i64): $str

The text for errno `code`, as a new string the caller owns
(`"Unknown error: N"` for a code the C library has no text for).

```toke
m=example;
i=io:std.io;
i=o:std.os;

f=main():i64{
  let fd=o.open("/nonexistent/x";o.ordonly();0);
  if(fd<0){
    io.eprintln(o.strerror(o.errno()));
    <1
  };
  o.close(fd);
  <0
};
```

### Flag and descriptor constants

Zero-argument functions, so the values are the host's own.

| Function | Value |
|----------|-------|
| `os.ordonly(): i64` | `O_RDONLY` |
| `os.owronly(): i64` | `O_WRONLY` |
| `os.ordwr(): i64` | `O_RDWR` |
| `os.ocreat(): i64` | `O_CREAT` |
| `os.otrunc(): i64` | `O_TRUNC` |
| `os.oappend(): i64` | `O_APPEND` |
| `os.onofollow(): i64` | `O_NOFOLLOW` (136.77). `os.open` already adds it to every open (AMB-07): a symlinked final path component fails with errno `ELOOP` whatever the flags, so passing it changes nothing there; it is exported so a flag set can be spelled and compared in full |
| `os.stdinfd(): i64` | `0` |
| `os.stdoutfd(): i64` | `1` |
| `os.stderrfd(): i64` | `2` |

## See Also

- [std.file](/docs/stdlib/file/) -- whole-file reads and writes with typed errors
- [std.io](/docs/stdlib/io/) -- console text, including `io.eprint` for stderr without a newline
- [std.env](/docs/stdlib/env/) -- environment variables with typed errors
