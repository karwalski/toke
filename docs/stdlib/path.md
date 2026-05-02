---
title: std.path -- Path Operations
slug: path
section: reference/stdlib
order: 29
---

## Overview

The `std.path` module provides pure path string manipulation functions. All operations are performed on strings only -- no filesystem calls are made, and no I/O occurs. Paths are treated as POSIX-style UTF-8 strings. Functions that return strings return new allocations; callers own the result.

## Functions

### path.join(a: $str; b: $str): $str

Joins two path segments with exactly one `'/'` between them. Trailing slashes on `a` and leading slashes on `b` are stripped before joining.

**Example:**
```toke
let p = path.join("foo/bar"; "baz.tk");   -- p = "foo/bar/baz.tk"
let q = path.join("foo/bar/"; "/baz.tk"); -- q = "foo/bar/baz.tk"
let r = path.join(""; "baz");             -- r = "/baz"
```

### path.ext(p: $str): $str

Returns the extension of the last path component, including the leading dot (e.g. `".tk"`). Returns `""` if there is no extension.

Special cases:
- `".hidden"` (pure dotfile with no second dot) -> `".hidden"` (the whole basename is the extension)
- `"foo.tar.gz"` -> `".gz"` (only the last extension)
- `"foo."` -> `"."`

**Example:**
```toke
let e1 = path.ext("pages/index.tk");  -- e1 = ".tk"
let e2 = path.ext("archive.tar.gz");  -- e2 = ".gz"
let e3 = path.ext(".hidden");         -- e3 = ".hidden"
let e4 = path.ext("readme");          -- e4 = ""
```

### path.stem(p: $str): $str

Returns the filename without its extension. The stem of a pure dotfile (e.g. `".hidden"`) is `""`.

**Example:**
```toke
let s1 = path.stem("pages/foo.tk");   -- s1 = "foo"
let s2 = path.stem("foo.tar.gz");     -- s2 = "foo.tar"
let s3 = path.stem(".hidden");        -- s3 = ""
let s4 = path.stem("readme");         -- s4 = "readme"
```

### path.dir(p: $str): $str

Returns the parent directory of the path. Equivalent to POSIX `dirname`.

Special cases:
- A bare filename with no slash -> `"."`
- `"/"` -> `"/"`
- `"/foo"` -> `"/"`

**Example:**
```toke
let d1 = path.dir("foo/bar.tk");  -- d1 = "foo"
let d2 = path.dir("bar.tk");      -- d2 = "."
let d3 = path.dir("/");           -- d3 = "/"
let d4 = path.dir("/usr/bin");    -- d4 = "/usr"
```

### path.base(p: $str): $str

Returns the last component of the path. Equivalent to POSIX `basename`.

Special cases:
- `"/"` -> `"/"`
- `""` -> `""`

**Example:**
```toke
let b1 = path.base("foo/bar.tk");  -- b1 = "bar.tk"
let b2 = path.base("/usr/bin");    -- b2 = "bin"
let b3 = path.base("/");           -- b3 = "/"
let b4 = path.base("");            -- b4 = ""
```

### path.isabs(p: $str): bool

Returns `true` if the path is absolute (starts with `'/'`), `false` otherwise.

**Example:**
```toke
let y = path.isabs("/usr/local");  -- y = true
let n = path.isabs("relative/p");  -- n = false
let z = path.isabs("");            -- z = false
```
