---
title: std.toml -- TOML 1.0 Parsing
slug: toml
section: reference/stdlib
order: 38
---

## Overview

The `std.toml` module provides TOML 1.0 parsing via FFI to the tomlc99 library (Tom Pawlak, MIT licence, vendored at `stdlib/vendor/tomlc99/`). It is the standard way to read `ooke.toml` configuration files and any other TOML-formatted data in toke programs.

Parsed tables are returned as opaque `$tomlval` handles. Values are extracted by key using type-specific accessor functions. A document returned by `toml.load` or `toml.loadfile` owns its whole tree and is released with `toml.free` when no longer needed; a sub-table returned by `toml.section` or `toml.sectionarr` is a non-owning view into its document, is released with it, and must not be used after it. `toml.free` on a view does nothing.

All functions return a result type of the form `ok!$tomlerr`. Check for error with standard toke `|{...}` or `!` propagation.

## Functions

### toml.load(src: $str): $tomlval!$tomlerr

Parses a TOML document from the string `src`. Returns an opaque `$tomlval` handle on success, or `$tomlerr` if the document is malformed.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=demo():i64{
  let src = "[server]\nhost = \"localhost\"\nport = 8080\n";
  let cfg = mt toml.load(src) {$ok:v v;$err:e < 1};
  < 0
};
```

### toml.loadfile(path: $str): $tomlval!$tomlerr

Reads the file at `path` and parses its contents as TOML. Returns `$tomlerr` if the file cannot be opened or the contents are not valid TOML.

Requires the `fs.read` capability (`--allow-read`, or `fs_read = true` in `tkc.toml [capabilities]`); without it the program stops with `CAP001`. `toml.load` parses a string and needs no grant.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=demo():i64{
  let cfg = mt toml.loadfile("ooke.toml") {$ok:v v;$err:e < 1};
  < 0
};
```

### toml.str(v: $tomlval; key: $str): $str!$tomlerr

Retrieves the string value associated with `key` in the TOML table `v`. Returns `$tomlerr` with `msg` `key not found` if the key is absent, or `key is not a string` if its value has another type.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("[server]\nhost = \"localhost\"\n") {$ok:v v;$err:e < 1};
  let host = mt toml.str(cfg; "host") {$ok:v v;$err:e < 1};
  < 0
};
```

### toml.i64(v: $tomlval; key: $str): i64!$tomlerr

Retrieves the integer value associated with `key` in the TOML table `v`. Returns `$tomlerr` with `msg` `key not found` if the key is absent, or `key is not an integer` if its value has another type.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("[server]\nport = 8080\n") {$ok:v v;$err:e < 1};
  let port = mt toml.i64(cfg; "port") {$ok:v v;$err:e < 1};
  < port
};
```

### toml.bool(v: $tomlval; key: $str): bool!$tomlerr

Retrieves the boolean value associated with `key` in the TOML table `v`. Returns `$tomlerr` with `msg` `key not found` if the key is absent, or `key is not a boolean` if its value has another type.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("debug = true\n") {$ok:v v;$err:e < 1};
  let debug = mt toml.bool(cfg; "debug") {$ok:v v;$err:e < 1};
  < 0
};
```

### toml.f64(v: $tomlval; key: $str): f64!$tomlerr

Retrieves the floating-point value associated with `key` in the TOML table `v` (121.68). A float (`ratio = 1.5`, `inf`, `nan`) is returned as is; a decimal integer (`ratio = 1`) is read as the nearest `f64`, so a config author need not write `1.0`. Returns `$tomlerr` with `msg` `key not found` if the key is absent, or `key is not a float` if its value is a string, boolean, date, array or table. A stored `0.0` is a success: the `$ok` arm is chosen from the error slot, not from the value.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("[render]
scale = 1.5
") {$ok:v v;$err:e < 1};
  let r = mt toml.section(cfg; "render") {$ok:v v;$err:e < 1};
  let scale = mt toml.f64(r; "scale") {$ok:v v;$err:e 1.0};
  if(scale>1.0){<1};
  < 0
};
```

### toml.has(v: $tomlval; key: $str): bool

`true` if `key` is present in the TOML table `v`, whatever its value -- a string, number, boolean, date, array or sub-table -- else `false` (121.68). It is the way to tell a key that is absent from one whose value an accessor does not read (an array, say): every accessor fails on both. `key` is one key of `v`, not a dotted path: for `[server] port = 80`, `toml.has(cfg; "server.port")` is `false`; use `toml.section(cfg; "server")` and then `toml.has(server; "port")`. Infallible.

**Example:**
```toke
m=example;
i=toml:std.toml;
i=io:std.io;

f=main():i64{
  let cfg = mt toml.load("plugins = [\"a\", \"b\"]\n") {$ok:v v;$err:e < 1};
  if(toml.has(cfg; "plugins")){io.println("plugins configured")};
  < 0
};
```

### toml.section(v: $tomlval; key: $str): $tomlval!$tomlerr

Returns a `$tomlval` handle for the sub-table identified by `key`. The returned handle is a non-owning view into the parent table -- do not use it after the parent has been released. Returns `$tomlerr` with `msg` `key not found` if the key is absent, or `key is not a table` if its value is not a table.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("[server]\nhost = \"localhost\"\n") {$ok:v v;$err:e < 1};
  let server = mt toml.section(cfg; "server") {$ok:v v;$err:e < 1};
  let host   = mt toml.str(server; "host") {$ok:v v;$err:e < 1};
  < 0
};
```

### toml.strarr(v: $tomlval; key: $str): @$str!$tomlerr
### toml.i64arr(v: $tomlval; key: $str): @i64!$tomlerr
### toml.f64arr(v: $tomlval; key: $str): @f64!$tomlerr
### toml.boolarr(v: $tomlval; key: $str): @bool!$tomlerr

Reads the array at `key` in the TOML table `v` as a toke array whose every element has the named type (113.B.4). An element is accepted exactly when the same value under a key would be by `toml.str`, `toml.i64`, `toml.f64` or `toml.bool` (so `toml.f64arr` reads a decimal integer element as the nearest `f64`). An empty array (`x = []`) is a success with length `0`. Returns `$tomlerr` with `msg` `key not found` if the key is absent, `key is not an array` if its value is not an array, or `array element N is not a string` (`an integer`, `a float`, `a boolean`; `N` counts from `0`) for the first element of another type -- TOML 1.0 allows mixed arrays, and those are refused rather than read in part.

**Example:**
```toke
m=example;
i=toml:std.toml;
i=io:std.io;
i=s:std.str;

f=main():i64{
  let cfg = mt toml.load("plugins = [\"sitemap\", \"feed\"]\nports = [80, 443]\nratios = [0.5, 2]\n") {$ok:v v;$err:e < 1};
  let plugins = mt toml.strarr(cfg; "plugins") {$ok:v v;$err:e < 1};
  io.println(plugins.get(0));
  let ports = mt toml.i64arr(cfg; "ports") {$ok:v v;$err:e < 1};
  let ratios = mt toml.f64arr(cfg; "ratios") {$ok:v v;$err:e < 1};
  io.println(s.fromfloat(ratios.get(0) * 2.0));
  < ports.len as i64
};
```

### toml.sectionarr(v: $tomlval; key: $str): @$tomlval!$tomlerr

Reads an array of tables -- the `[[route]]` form -- at `key` in `v` (113.B.4). Each element is a non-owning view into the document, read with the same accessors as a `toml.section` result. Errors are as for `toml.strarr`, with `array element N is not a table`.

**Example:**
```toke
m=example;
i=toml:std.toml;
i=io:std.io;

f=main():i64{
  let cfg = mt toml.load("[[route]]\npath = \"/\"\n[[route]]\npath = \"/blog\"\n") {$ok:v v;$err:e < 1};
  let routes = mt toml.sectionarr(cfg; "route") {$ok:v v;$err:e < 1};
  let i = mut.0;
  lp(i<routes.len){
    let path = mt toml.str(routes.get(i); "path") {$ok:v v;$err:e < 1};
    io.println(path);
    i = i + 1
  };
  < 0
};
```

### toml.keys(v: $tomlval): @$str

The keys of the TOML table `v`, whatever their values (113.B.4): plain values first, then arrays (including arrays of tables), then sub-tables, each group in document order. Like `toml.has`, these are the keys of `v` itself, not dotted paths. The strings are copies, so a key list stays readable after `toml.free`. Infallible.

### toml.len(v: $tomlval): u64

The number of keys in the TOML table `v` -- `toml.keys(v).len` without building the list (113.B.4). Infallible.

**Example:**
```toke
m=example;
i=toml:std.toml;
i=io:std.io;

f=main():i64{
  let cfg = mt toml.load("a = 1\nb = [2]\n[c]\nd = 3\n") {$ok:v v;$err:e < 1};
  let ks = toml.keys(cfg);
  io.println(ks.get(0));
  < toml.len(cfg) as i64
};
```

### toml.free(v: $tomlval): bool

Releases a document returned by `toml.load` or `toml.loadfile`, with every section view into it (113.B.4). Returns `true` when `v` was a live document and is now freed; `false`, and nothing happens, for a section view, an element of `toml.sectionarr`, or a document already freed -- so a double free, or freeing a view, cannot corrupt the document. After `toml.free` the document and every view into it must not be used. Strings already read from it (`toml.str`, `toml.strarr`, `toml.keys`) are copies and stay valid.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("port = 8080\n") {$ok:v v;$err:e < 1};
  let port = mt toml.i64(cfg; "port") {$ok:v v;$err:e < 1};
  let freed = toml.free(cfg);
  < port
};
```

## Full Example

Loading `ooke.toml` and reading configuration values:

```toke
m=example;
i=toml:std.toml;
i=log:std.log;
i=str:std.str;

f=main():i64{
  let cfg = mt toml.loadfile("ooke.toml") {$ok:v v;$err:e < 1};

  let server  = mt toml.section(cfg; "server") {$ok:v v;$err:e < 1};
  let host    = mt toml.str(server;  "host") {$ok:v v;$err:e < 1};
  let port    = mt toml.i64(server;  "port") {$ok:v v;$err:e < 1};
  let debug   = mt toml.bool(server; "debug") {$ok:v v;$err:e < 1};

  let db      = mt toml.section(cfg; "db") {$ok:v v;$err:e < 1};
  let dburl  = mt toml.str(db; "url") {$ok:v v;$err:e < 1};

  log.info(str.concat("listening on "; str.concat(host; str.concat(":"; str.fromint(port)))); @());
  < 0
};
```

## Error Types

### $tomlerr

Returned by all `std.toml` functions when an operation fails.

| Field | Type | Meaning |
|-------|------|---------|
| msg   | $str | Human-readable description of the error |

`msg` is the parser's own message (for a malformed document, e.g. `line 1: syntax error`) or the accessor's reason (`key not found`, `key is not an integer`, `array element 2 is not a string`). It is readable in the `$err` arm, and it survives `!` propagation into a function whose error type has the same layout — `$tomlerr` itself, or a record such as `t=$cfgerr{msg:$str}`. Propagated into any other error type it arrives without a payload (story 127.182; see `docs/runtime-abi.md` §7.3).

```toke
m=example;
i=toml:std.toml;
i=io:std.io;

f=report():i64{
  mt toml.load("x = = 1") {$ok:v io.println("ok");$err:e io.println(e.msg)};
  < 0
};
```
