# std.file -- File System Operations

## Overview

The `std.file` module provides functions for reading, writing, and managing files on the local file system. All paths are UTF-8 strings. Operations that can fail return a result type with `FileErr`.

## Functions

### file.read(path: Str): Str!FileErr

Reads the entire contents of the file at `path` and returns it as a string. Returns `FileErr.NotFound` if the file does not exist, `FileErr.Permission` if access is denied, or `FileErr.IO` on other I/O failures.

**Example:**
```toke
let content = file.read("/tmp/data.txt");
(* content = ok("hello world") *)
```

### file.write(path: Str; content: Str): bool!FileErr

Writes `content` to the file at `path`, creating the file if it does not exist and truncating it if it does. Returns `true` on success.

**Example:**
```toke
let ok = file.write("/tmp/data.txt"; "hello world");
(* ok = ok(true) *)
```

### file.append(path: Str; content: Str): bool!FileErr

Appends `content` to the end of the file at `path`, creating the file if it does not exist. Returns `true` on success.

**Example:**
```toke
file.write("/tmp/log.txt"; "line 1\n");
file.append("/tmp/log.txt"; "line 2\n");
let content = file.read("/tmp/log.txt");
(* content = ok("line 1\nline 2\n") *)
```

### file.exists(path: Str): bool

Returns `true` if a file exists at `path`, `false` otherwise. This function is infallible.

**Example:**
```toke
let y = file.exists("/tmp/data.txt");  (* y = true *)
let n = file.exists("/tmp/nope.txt");  (* n = false *)
```

### file.delete(path: Str): bool!FileErr

Deletes the file at `path`. Returns `true` on success. Returns `FileErr` if the file cannot be deleted.

**Example:**
```toke
file.write("/tmp/temp.txt"; "data");
let ok = file.delete("/tmp/temp.txt");
(* ok = ok(true) *)
```

### file.list(dir: Str): [Str]!FileErr

Returns an array of filenames in the directory `dir`. Returns `FileErr.NotFound` if the directory does not exist.

**Example:**
```toke
let entries = file.list("/tmp");
(* entries = ok(["file1.txt"; "file2.txt"; ...]) *)
```

## Error Types

### FileErr

A sum type representing file operation failures.

| Variant | Field Type | Meaning |
|---------|------------|---------|
| NotFound | Str | The file or directory does not exist |
| Permission | Str | The process lacks permission to perform the operation |
| IO | Str | A general I/O error occurred |
