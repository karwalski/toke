---
title: std.file
slug: file
section: reference/stdlib
order: 15
---

**Status: Implemented** -- C runtime backing.

The `std.file` module provides functions for reading, writing, and managing files and directories on the local file system. All paths are UTF-8 strings. Operations that can fail return a result type with `$fileerr`.

> **Text or bytes — pick deliberately.** `file.read` and `file.write` carry a
> `$str`, which is NUL-terminated. They stop at the first zero byte, and they
> do it **silently**: a 12 MB PDF read with `file.read` comes back as a few
> bytes with no error. Use them for text only. For anything binary — an
> archive, a PDF, an image, a spreadsheet, a compiled artefact — use
> **`file.readbytes` / `file.writebytes`**, which carry an exact `@(byte)` with
> the length beside the data.

> **Implemented functions (from `file.tki`):** `file.read`, `file.write`, `file.append`, `file.exists`, `file.delete`, `file.list`, `file.isdir`, `file.mkdir`, `file.copy`, `file.listall`, `file.readbytes`, `file.writebytes`, `file.readrange`, `file.size`, `file.realpath`. Functions documented in earlier versions (`mkdir_p`, `rmdir`, `rmdir_r`, `is_file`, `move`, `mtime`, `join`, `basename`, `dirname`, `absolute`, `ext`, `readlines`, `glob`) are not in the current tki. (`size` returned to the interface in 135.12, with an implementation that refuses what it cannot measure; `absolute` returned in 121.60 as `file.realpath`, with its failure reported.)

## Types

### $fileerr

A sum type representing file operation failures. The `$err` arm of every
fallible `file.*` call binds it: the variant says what went wrong, and every
variant carries a message naming the call and the path.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $notfound | $str | The file or directory does not exist, or a component of the path is not a directory |
| $permission | $str | The process lacks permission to perform the operation |
| $io | $str | A general I/O error occurred (disk full, interrupted, a short read, a symlink loop in a parent directory of the path, etc.) |
| $invalid | $str | The operation does not apply to what is there (for example `file.mkdir` over an existing file) |
| $isdir | $str | The path is a directory |
| $notregular | $str | A fifo, socket or device — its size is not its content; or a fifo written to with no process reading it |
| $symlink | $str | The final component is a symlink, which `std.file` does not follow (AMB-07) |
| $toolarge | $str | Over the 64 MiB limit of `file.readbytes` |
| $nomem | $str | It fits the limit, but the allocation failed |
| $badarg | $str | A null path, content or byte array reached the call |

**Branch on the variant, not on the message**: the variants are the interface
and the wording is not. The interface file spells this type `FileErr`, which
toke source cannot name in a parameter, so match it where the `$err` arm binds
it rather than passing it on. Until 127.102b the type carried only the first
three variants and the rest were readable only through `file.lasterrkind()`,
which is retired along with `file.lasterr()`.

## Functions

### file.read(path: $str): $str!$fileerr

Reads the entire contents of the file at `path` and returns it as a UTF-8 string. Returns `$fileerr.$notfound` if the file does not exist, `$fileerr.$permission` if access is denied, or `$fileerr.$io` on other I/O failures.

A directory is `$fileerr.$isdir`, as it is for `file.readbytes` and `file.size`; until 121.65 `file.read` on a directory returned `$ok ""`. A final-component symlink is `$fileerr.$symlink` (AMB-07). An empty file is `$ok ""`, not an error.

```toke
m=example;
i=file:std.file;

f=readfile():$str{
  let r=file.read("/tmp/data.txt");
  <mt r {
    $ok:s  s;
    $err:e ""
  }
};
```

### file.write(path: $str; content: $str): bool!$fileerr

Writes `content` to the file at `path`, creating the file if it does not exist and truncating it if it does. Returns `true` on success.

If `path` is a fifo that no process has open for reading, the call fails at once with `$fileerr.$notregular` rather than waiting for a reader (121.77). With a reader there, every byte is written.

A write that fails after the call has started -- a full disk (ENOSPC), or a fifo whose reader closes before all the data is through (EPIPE) -- is `$fileerr.$io`, with the cause in the message; it is never reported as success, including when the failure only shows when the data is flushed and the file closed (121.84). The fifo case does not kill the program with SIGPIPE: the call holds SIGPIPE off in the calling thread for the duration of the write and restores that thread's signal mask afterwards; the program's SIGPIPE disposition is not changed.

```toke
m=example;
i=file:std.file;

f=writefile():i64{
  <mt file.write("/tmp/data.txt";"hello world") {
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.append(path: $str; content: $str): bool!$fileerr

Appends `content` to the end of the file at `path`, creating the file if it does not exist. Returns `true` on success; does not truncate existing content.

A fifo with no reader is `$fileerr.$notregular` at once, and a full disk or a reader that closes early is `$fileerr.$io`, as for `file.write`.

```toke
m=example;
i=file:std.file;

f=appendfile():i64{
  <mt file.append("/tmp/log.txt";"new line\n") {
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.exists(path: $str): bool

Returns `true` if a file or directory exists at `path`, `false` otherwise. This function is infallible.

```toke
m=example;
i=file:std.file;

f=checkfile():bool{
  let y=file.exists("/tmp/data.txt");
  < y
}
```

### file.delete(path: $str): bool!$fileerr

Deletes the file at `path`. Returns `true` on success. Returns `$fileerr.$notfound` if the file does not exist, or `$fileerr.$permission` if access is denied.

```toke
m=example;
i=file:std.file;

f=delfile():i64{
  <mt file.delete("/tmp/temp.txt") {
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.list(dir: $str): @($str)!$fileerr

Returns an array of entry names (files and subdirectories, not full paths) in the directory `dir`. Returns `$fileerr.$notfound` if the directory does not exist, or `$fileerr.$permission` if access is denied.

```toke
m=example;
i=file:std.file;

f=listdir():i64{
  <mt file.list("/tmp") {
    $ok:entries 0;
    $err:e      1
  }
};
```

### file.listall(dir: $str): @($str)!$fileerr

Returns all file paths under `dir` recursively as full paths. Returns `$fileerr.$notfound` if the directory does not exist.

```toke
m=example;
i=file:std.file;

f=listrecursive():i64{
  <mt file.listall("/tmp") {
    $ok:entries 0;
    $err:e      1
  }
};
```

### file.isdir(path: $str): bool

Returns `true` if `path` refers to a directory, `false` for any other entry type or if the path does not exist. This function is infallible.

```toke
m=example;
i=file:std.file;

f=checkdir():bool{
  let d=file.isdir("/tmp");
  < d
}
```

### file.mkdir(path: $str): bool!$fileerr

Creates a single directory at `path`. Returns `$fileerr.$io` if the parent directory does not exist.

```toke
m=example;
i=file:std.file;

f=makedir():i64{
  <mt file.mkdir("/tmp/mydir") {
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.copy(src: $str; dst: $str): bool!$fileerr

Copies the file at `src` to `dst`, creating `dst` if it does not exist and overwriting it if it does. Returns `$fileerr.$notfound` if `src` does not exist.

A `dst` that is a fifo with no reader is `$fileerr.$notregular` at once; a `src` that is a fifo with no writer copies as empty (121.77). A full disk or a `dst` fifo whose reader closes early is `$fileerr.$io`, as for `file.write`. The error message names the path that failed: `dst` when opening, writing or closing the destination failed, `src` otherwise (121.84).

```toke
m=example;
i=file:std.file;

f=copyfile():i64{
  <mt file.copy("/tmp/original.txt";"/tmp/backup.txt") {
    $ok:ok 0;
    $err:e 1
  }
};
```

### file.readbytes(path: $str): @(byte)!$fileerr

Reads the whole file at `path` and returns its exact bytes. Nothing is
interpreted, nothing terminates the data, and a zero byte is just a zero byte.

**An empty result is never a failure.** A file of length zero reads back as a
zero-length `@(byte)` through the `$ok` arm. Failure takes the `$err` arm,
which binds the reason as a `$fileerr` variant. The outcomes:

| Arm | Meaning |
|---|---|
| `$ok` | the bytes are exact; length may legitimately be 0 |
| `$notfound` | no such path, or a component of it is not a directory |
| `$permission` | the process may not open it |
| `$isdir` | the path is a directory |
| `$notregular` | a fifo, socket or device — its size is not its content |
| `$symlink` | the final component is a symlink, which `std.file` does not follow (AMB-07) |
| `$toolarge` | over the 64 MiB limit (see below) |
| `$nomem` | it fits the limit, but the allocation failed |
| `$io` | `read(2)` failed, or the file changed size mid-read |
| `$badarg` | a null path |

**The 64 MiB limit is real, not a formality.** A toke `@(byte)` stores one
`i64` per byte, so an *N*-byte file costs *8N* in the array plus *N* while it is
staged: at the limit that is roughly 576 MiB resident for one call. A larger
file is **refused** with `$toolarge` rather than read partially — a short read is
reported as `$io`, never returned as a shorter array.

To read a file bigger than that, use **`file.readrange`** below, which caps the
*window* and not the *file*.

```toke
m=example;
i=file:std.file;
i=io:std.io;
i=s:std.str;

f=loadblob(path:$str):i64{
  mt file.readbytes(path) {
    $ok:b  io.println(s.concat("bytes=";s.fromint(b.len)));
    $err:e io.println(s.concat("failed: ";mt e {
      $notfound:m m;$permission:m m;$io:m m;$invalid:m m;$isdir:m m;
      $notregular:m m;$symlink:m m;$toolarge:m m;$nomem:m m;$badarg:m m
    }))
  };
  <0
};
```

Branch on the variant, not on the message: the variant is the interface and
the wording is not.

```toke
m=example;
i=file:std.file;

f=report(path:$str):i64{
  <mt file.readbytes(path) {
    $ok:b  0;
    $err:e mt e {
      $notfound:m 1;$permission:m 2;$io:m 3;$invalid:m 3;$isdir:m 3;
      $notregular:m 3;$symlink:m 3;$toolarge:m 3;$nomem:m 3;$badarg:m 3
    }
  }
};
```

### file.writebytes(path: $str; data: @(byte)): bool!$fileerr

Writes the exact bytes of `data` to `path`, creating the file if it does not
exist and truncating it if it does. Zero bytes are written like any other. This
is the counterpart to `file.readbytes`: without it a decompressed archive entry
(`zip.read` hands back an exact `@(byte)`) could be read but not saved, because
`file.write` would stop at the first zero.

A zero-length `@(byte)` writes an empty file and succeeds. Outcomes are the same
set as above: `$notfound` (a parent directory is missing), `$permission`, `$isdir`,
`$notregular`, `$symlink`, `$io`, `$badarg`.

**There is no atomic replace.** The file is opened create-or-truncate, mode
`0644`, and written in place, exactly like `file.write`. A failure partway
through therefore leaves a partially written file; the `$io` message says how
many bytes reached the disk, and deleting the remains is the caller's decision.

```toke
m=example;
i=file:std.file;

f=copyexact(src:$str;dst:$str):i64{
  <mt file.readbytes(src) {
    $ok:b  save(dst;b);
    $err:e 1
  }
};

f=save(dst:$str;b:@(byte)):i64{
  <mt file.writebytes(dst;b) {
    $ok:v  0;
    $err:e 1
  }
};
```

### file.readrange(path: $str; offset: i64; len: i64): @(byte)!$fileerr

Reads at most `len` bytes starting at `offset`, **without reading the rest of
the file**. This is how to read a file that `file.readbytes` refuses.

**The window is capped; the file is not.** `file.readbytes` caps the file
because it materialises all of it, and those two limits used to be one number.
They are not the same thing, and only the window costs memory — so `len` is
refused above 64 MiB with `toolarge`, and **the file's own size is not checked
at all**. A 2 GiB PDF is readable, in windows, at the same peak cost per call.

**A short result is an end, not a failure.** If the range runs past the end of
the file, the bytes that exist are returned and `b.len` says how many. An
`offset` at or beyond the end returns a real zero-length `@(byte)` with kind
`ok` — never a failure, and never confusable with one, which is what lets a
caller walk to the end of a file without knowing its size first. A short read
from *inside* the file (it shrank mid-read) is still `io`, because that is a
wrong answer rather than an end.

Outcomes add `badarg` for a negative `offset` or `len` and re-use `toolarge`
for an over-cap **window**; the rest are `file.readbytes`' outcomes exactly.

**Why a range and not a stream.** The formats this exists for are indexed from
their own end and reached by byte offset — a PDF through the `startxref`
pointer in its trailer, a zip (so every XLSX) through the end-of-central-
directory record. Neither can be parsed by reading forward from the start. A
sequential scan is four lines of toke on top of this call; random access cannot
be built on top of a stream at all.

```toke
m=example;
i=file:std.file;
i=io:std.io;
i=s:std.str;

(* The trailer-first access pattern: find the end, then read backwards from
   it. This is what a PDF or zip parser does, and why file.size exists. *)
f=readtrailer(path:$str):i64{
  <mt file.size(path) {
    $ok:n  tail(path;n);
    $err:e 1
  }
};

f=tail(path:$str;n:i64):i64{
  let from=mut.0;
  if(n>1024){ from=n-1024 };
  <mt file.readrange(path;from;1024) {
    $ok:b  io.println(s.concat("trailer bytes=";s.fromint(b.len)));
    $err:e 1
  }
};
```

### file.size(path: $str): i64!$fileerr

The number of bytes `file.readrange` can return from `path`, taken by `fstat`
on an opened descriptor rather than by `stat` on the path — so it cannot
disagree with the read calls about which file it measured, and it does not
follow a final symlink that those calls would refuse (AMB-07).

**Zero is a real size.** An empty file returns `0` on the `$ok` arm. This is
the one call in the module whose success value collides with the value-sentinel
convention the others use for failure, so it is discriminated on the error slot
instead ([`runtime-abi.md` §7.2](https://github.com/karwalski/toke/blob/main/docs/runtime-abi.md): *"the zero filler is not
the discriminant"*). A directory and a fifo are **refused** rather than
measured — `st_size` on either is not a count of readable bytes, and returning
it would be a plausible wrong answer.

Outcomes: `$ok`, `$notfound`, `$permission`, `$isdir`, `$notregular`, `$symlink`,
`$io`, `$badarg`. `$toolarge` and `$nomem` cannot occur, because nothing is
allocated and nothing is read.

> `file.stat` shares this same implementation, so the two cannot disagree about
> the answer. They still differ in how they report failure — `file.stat` keeps
> the value sentinel, so it cannot tell an empty file from an error. Prefer
> `file.size`.

### file.realpath(path: $str): $str!$fileerr

The absolute path `path` names, with every `.`, `..` and symlink resolved
(realpath(3)). A relative `path` is resolved against the working directory.
Every component must exist.

This is the one `std.file` call that **follows symlinks**, the last component
included: resolving them is what it is for. To keep a read inside a directory,
resolve both and compare prefixes -- a lexical check (`std.path`) cannot see a
symlinked directory that points outside. It opens nothing, so AMB-07 (which
refuses to *open* through a final-component symlink) does not apply.

| Variant | When |
|---------|------|
| `$notfound` | a component does not exist (the empty path included) |
| `$invalid` | a non-final component is not a directory |
| `$symlink` | a symlink cycle, or too long a chain ("too many levels of symbolic links") |
| `$permission` | a directory on the path cannot be searched |
| `$badarg` | a null path |

Resolving reads the directories on the path, so the call needs the `fs.read`
grant, as `file.exists` and `file.isdir` do.

```toke
m=example;
i=file:std.file;
i=s:std.str;

f=inside(root:$str;p:$str):bool{
  let r=mt file.realpath(root) {$ok:t t;$err:e ""};
  let q=mt file.realpath(p) {$ok:t t;$err:e ""};
  if(s.len(r)==0){<false};
  <s.startswith(q;s.concat(r;"/"))
};
```

### Retired: file.lasterr() and file.lasterrkind()

Both accessors were retired by 127.102b, an owner-authorised breaking change
landed in one release with `csv`'s (127.102a) and `zip`'s (127.102c). They
existed because the compiled error ABI once bound no payload; it now does, and
every fallible call in this module raises a real `$fileerr`. Replace
`file.lasterrkind()` with a match on the variant the `$err` arm binds, and
`file.lasterr()` with that variant's message. The error belongs to the call
rather than the process: a `let`-bound result keeps its own variant, and its
message stays exact across the next seven failures on the same thread. It is
set by every fallible call, not only the byte calls the accessors covered.

## Usage Examples

List a directory, filter to `.tk` files, read each one, count lines, and print a summary:

```toke
m=tksummary;
i=file:std.file;
i=str:std.str;
i=log:std.log;

f=countlines(content:$str):i64{
  let lines=str.split(content;"\n");
  < lines.len
};

f=summarize(dir:$str):i64{
  let entries=mt file.list(dir) {
    $ok:e  e;
    $err:e @()
  };

  let total=mut.0;
  lp(let i=0;i<entries.len;i=i+1){
    let name=mt entries.get(i) {$ok:v v;$err:e ""};
    let path=str.concat(dir;str.concat("/";name));
    let r=file.read(path);
    let n=mt r {$ok:content countlines(content);$err:e 0};
    total=total+n;
    log.info(name;@())
  };
  < 0
};
```

## See Also

- [std.str](/docs/stdlib/str) -- string manipulation for processing file content
- [std.json](/docs/stdlib/json) -- parse JSON files after reading them with `file.read`
- [std.log](/docs/stdlib/log) -- log file paths and operation outcomes
