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

> **Implemented functions (from `file.tki`):** `file.read`, `file.write`, `file.append`, `file.exists`, `file.delete`, `file.list`, `file.isdir`, `file.mkdir`, `file.copy`, `file.listall`, `file.readbytes`, `file.writebytes`, `file.lasterr`, `file.lasterrkind`. Functions documented in earlier versions (`mkdir_p`, `rmdir`, `rmdir_r`, `is_file`, `move`, `size`, `mtime`, `join`, `basename`, `dirname`, `absolute`, `ext`, `readlines`, `glob`) are not in the current tki.

## Types

### $fileerr

A sum type representing file operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| $notfound | $str | The file or directory does not exist |
| $permission | $str | The process lacks permission to perform the operation |
| $io | $str | A general I/O error occurred (disk full, interrupted, etc.) |

## Functions

### file.read(path: $str): $str!$fileerr

Reads the entire contents of the file at `path` and returns it as a UTF-8 string. Returns `$fileerr.$notfound` if the file does not exist, `$fileerr.$permission` if access is denied, or `$fileerr.$io` on other I/O failures.

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
zero-length `@(byte)` with `file.lasterrkind()` equal to `"ok"`. Failure takes
the `$err` arm, which the compiled `T!E` ABI gives no payload, so the reason is
read back with the two accessors below. The nine outcomes:

| `file.lasterrkind()` | Meaning |
|---|---|
| `ok` | the bytes are exact; length may legitimately be 0 |
| `notfound` | no such path, or a component of it is not a directory |
| `permission` | the process may not open it |
| `isdir` | the path is a directory |
| `notregular` | a fifo, socket or device — its size is not its content |
| `symlink` | the final component is a symlink, which `std.file` does not follow (AMB-07) |
| `toolarge` | over the 64 MiB limit (see below) |
| `nomem` | it fits the limit, but the allocation failed |
| `io` | `read(2)` failed, or the file changed size mid-read |

**The 64 MiB limit is real, not a formality.** A toke `@(byte)` stores one
`i64` per byte, so an *N*-byte file costs *8N* in the array plus *N* while it is
staged: at the limit that is roughly 576 MiB resident for one call. A larger
file is **refused** with `toolarge` rather than read partially — a short read is
reported as `io`, never returned as a shorter array.

```toke
m=example;
i=file:std.file;
i=io:std.io;
i=s:std.str;

f=loadblob(path:$str):i64{
  mt file.readbytes(path) {
    $ok:b  io.println(s.concat("bytes=";s.fromint(b.len)));
    $err:e io.println(s.concat("failed: ";file.lasterr()))
  };
  <0
};
```

Branch on `file.lasterrkind()`, not on the message: the kind is the interface
and the wording is not.

```toke
m=example;
i=file:std.file;
i=str:std.str;

f=report(path:$str):i64{
  <mt file.readbytes(path) {
    $ok:b  0;
    $err:e classify(file.lasterrkind())
  }
};

f=classify(kind:$str):i64{
  if(str.eq(kind;"notfound")){ <1 };
  if(str.eq(kind;"permission")){ <2 };
  <3
};
```

### file.writebytes(path: $str; data: @(byte)): bool!$fileerr

Writes the exact bytes of `data` to `path`, creating the file if it does not
exist and truncating it if it does. Zero bytes are written like any other. This
is the counterpart to `file.readbytes`: without it a decompressed archive entry
(`zip.read` hands back an exact `@(byte)`) could be read but not saved, because
`file.write` would stop at the first zero.

A zero-length `@(byte)` writes an empty file and succeeds. Outcomes are the same
set as above: `notfound` (a parent directory is missing), `permission`, `isdir`,
`notregular`, `symlink`, `io`, `badarg`.

**There is no atomic replace.** The file is opened create-or-truncate, mode
`0644`, and written in place, exactly like `file.write`. A failure partway
through therefore leaves a partially written file; `file.lasterr()` says how
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

### file.lasterr(): $str

The message for the most recent `file.readbytes` / `file.writebytes` failure,
or `""` if the last one succeeded. It names the path and, where it helps, the
numbers — the size that exceeded the limit, how many bytes were written before
the write failed. **The wording is for reporting and is not an interface**;
branch on `file.lasterrkind()` instead.

### file.lasterrkind(): $str

One of `ok`, `notfound`, `permission`, `isdir`, `notregular`, `symlink`,
`toolarge`, `nomem`, `io`, `badarg` — the outcome of the most recent
`file.readbytes` / `file.writebytes` call. These tokens are stable.

Both accessors describe the last **byte** call only; the older text and
directory calls do not set them. The value is process-wide, so read it
immediately on taking the `$err` arm.

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
