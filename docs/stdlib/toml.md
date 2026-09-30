---
title: std.toml -- TOML 1.0 Parsing
slug: toml
section: reference/stdlib
order: 38
---

## Overview

The `std.toml` module provides TOML 1.0 parsing via FFI to the tomlc99 library (Tom Pawlak, MIT licence, vendored at `stdlib/vendor/tomlc99/`). It is the standard way to read `ooke.toml` configuration files and any other TOML-formatted data in toke programs.

Parsed tables are returned as opaque `$tomlval` handles. Values are extracted by key using type-specific accessor functions. Top-level tables returned by `toml.load` and `toml.loadfile` must be released when no longer needed; sub-tables returned by `toml.section` are non-owning views into their parent and must not be freed independently.

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

Retrieves the string value associated with `key` in the TOML table `v`. Returns `$tomlerr` if the key does not exist or its value is not a string.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("[server]\nhost = \"localhost\"\n") {$ok:v v;$err:e < 1};
  let host = mt toml.str(cfg; "host") {$ok:v v;$err:e ""};
  < 0
};
```

### toml.i64(v: $tomlval; key: $str): i64!$tomlerr

Retrieves the integer value associated with `key` in the TOML table `v`. Returns `$tomlerr` if the key does not exist or its value is not an integer.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("[server]\nport = 8080\n") {$ok:v v;$err:e < 1};
  let port = mt toml.i64(cfg; "port") {$ok:v v;$err:e 0};
  < port
};
```

### toml.bool(v: $tomlval; key: $str): bool!$tomlerr

Retrieves the boolean value associated with `key` in the TOML table `v`. Returns `$tomlerr` if the key does not exist or its value is not a boolean.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("debug = true\n") {$ok:v v;$err:e < 1};
  let debug = mt toml.bool(cfg; "debug") {$ok:v v;$err:e false};
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

Returns a `$tomlval` handle for the sub-table identified by `key`. The returned handle is a non-owning view into the parent table -- do not use it after the parent has been released. Returns `$tomlerr` if the key does not exist or is not a table.

**Example:**
```toke
m=example;
i=toml:std.toml;

f=main():i64{
  let cfg = mt toml.load("[server]\nhost = \"localhost\"\n") {$ok:v v;$err:e < 1};
  let server = mt toml.section(cfg; "server") {$ok:v v;$err:e < 1};
  let host   = mt toml.str(server; "host") {$ok:v v;$err:e ""};
  < 0
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
  let host    = mt toml.str(server;  "host") {$ok:v v;$err:e ""};
  let port    = mt toml.i64(server;  "port") {$ok:v v;$err:e 0};
  let debug   = mt toml.bool(server; "debug") {$ok:v v;$err:e false};

  let db      = mt toml.section(cfg; "db") {$ok:v v;$err:e < 1};
  let dburl  = mt toml.str(db; "url") {$ok:v v;$err:e ""};

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

`msg` is the parser's own message (for a malformed document, e.g. `line 1: syntax error`) or the accessor's reason (`key not found or not an integer`). It is readable in the `$err` arm, and it survives `!` propagation into a function whose error type has the same layout — `$tomlerr` itself, or a record such as `t=$cfgerr{msg:$str}`. Propagated into any other error type it arrives without a payload (story 127.182; see `docs/runtime-abi.md` §7.3).

```toke
m=example;
i=toml:std.toml;
i=io:std.io;

f=report():i64{
  mt toml.load("x = = 1") {$ok:v io.println("ok");$err:e io.println(e.msg)};
  < 0
};
```
