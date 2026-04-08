# std.toml -- TOML 1.0 Parsing

## Overview

The `std.toml` module provides TOML 1.0 parsing via FFI to the tomlc99 library (Tom Pawlak, MIT licence, vendored at `stdlib/vendor/tomlc99/`). It is the standard way to read `ooke.toml` configuration files and any other TOML-formatted data in toke programs.

Parsed tables are returned as opaque `$tomlval` handles. Values are extracted by key using type-specific accessor functions. Top-level tables returned by `toml.load` and `toml.loadfile` must be released when no longer needed; sub-tables returned by `toml.section` are non-owning views into their parent and must not be freed independently.

All functions return a result type of the form `ok!$tomlerr`. Check for error with standard toke `match` or `!` propagation.

## Functions

### toml.load(src: Str): $tomlval!$tomlerr

Parses a TOML document from the string `src`. Returns an opaque `$tomlval` handle on success, or `$tomlerr` if the document is malformed.

**Example:**
```toke
let src = "[server]\nhost = \"localhost\"\nport = 8080\n";
let cfg = toml.load(src)!;
```

### toml.loadfile(path: Str): $tomlval!$tomlerr

Reads the file at `path` and parses its contents as TOML. Returns `$tomlerr` if the file cannot be opened or the contents are not valid TOML.

**Example:**
```toke
let cfg = toml.loadfile("ooke.toml")!;
```

### toml.str(v: $tomlval; key: Str): Str!$tomlerr

Retrieves the string value associated with `key` in the TOML table `v`. Returns `$tomlerr` if the key does not exist or its value is not a string.

**Example:**
```toke
let host = toml.str(cfg; "host")!;
```

### toml.i64(v: $tomlval; key: Str): i64!$tomlerr

Retrieves the integer value associated with `key` in the TOML table `v`. Returns `$tomlerr` if the key does not exist or its value is not an integer.

**Example:**
```toke
let port = toml.i64(cfg; "port")!;
```

### toml.bool(v: $tomlval; key: Str): bool!$tomlerr

Retrieves the boolean value associated with `key` in the TOML table `v`. Returns `$tomlerr` if the key does not exist or its value is not a boolean.

**Example:**
```toke
let debug = toml.bool(cfg; "debug")!;
```

### toml.section(v: $tomlval; key: Str): $tomlval!$tomlerr

Returns a `$tomlval` handle for the sub-table identified by `key`. The returned handle is a non-owning view into the parent table — do not use it after the parent has been released. Returns `$tomlerr` if the key does not exist or is not a table.

**Example:**
```toke
let server = toml.section(cfg; "server")!;
let host   = toml.str(server; "host")!;
```

## Full Example

Loading `ooke.toml` and reading configuration values:

```toke
import std.toml;
import std.log;

(* ooke.toml:
 *   [server]
 *   host  = "0.0.0.0"
 *   port  = 9000
 *   debug = false
 *
 *   [db]
 *   url = "postgres://localhost/ooke"
 *)

let cfg = match toml.loadfile("ooke.toml") {
    ok(v)  -> v
  | err(e) -> { log.error(e.msg); exit(1) }
};

let server  = toml.section(cfg; "server")!;
let host    = toml.str(server;  "host")!;
let port    = toml.i64(server;  "port")!;
let debug   = toml.bool(server; "debug")!;

let db      = toml.section(cfg; "db")!;
let db_url  = toml.str(db; "url")!;

log.info("Listening on " ++ host ++ ":" ++ str.from_int(port));
```

## Error Types

### $tomlerr

Returned by all `std.toml` functions when an operation fails.

| Field | Type | Meaning |
|-------|------|---------|
| msg   | Str  | Human-readable description of the error |
