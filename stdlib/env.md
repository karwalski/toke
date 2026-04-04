# std.env -- Environment Variables

## Overview

The `std.env` module provides functions for reading and writing process environment variables. Keys and values are UTF-8 strings. Keys must not be empty or contain `=` or NUL characters.

## Functions

### env.get(key: Str): Str!EnvErr

Looks up the environment variable `key`. Returns the value on success. Returns `EnvErr.NotFound` if the variable is not set, or `EnvErr.Invalid` if the key is empty or contains invalid characters.

**Example:**
```toke
let path = env.get("PATH");  (* path = ok("/usr/bin:...") *)
let e = env.get("UNSET_VAR"); (* e = err(EnvErr.NotFound{...}) *)
```

### env.get_or(key: Str; default: Str): Str

Looks up the environment variable `key`. Returns the value if it exists, or `default` if the variable is not set or the key is invalid. This function is infallible.

**Example:**
```toke
let port = env.get_or("PORT"; "8080");
(* port = "8080" if PORT is not set *)

let home = env.get_or("HOME"; "/tmp");
(* home = "/Users/alice" if HOME is set *)
```

### env.set(key: Str; val: Str): bool

Sets the environment variable `key` to `val` in the current process. Overwrites any existing value. Returns `true` on success, `false` if the key is empty or contains invalid characters.

**Example:**
```toke
env.set("APP_MODE"; "production");  (* returns true *)
let mode = env.get("APP_MODE");     (* mode = ok("production") *)

env.set(""; "val");     (* returns false -- empty key *)
env.set("BAD=KEY"; ""); (* returns false -- key contains '=' *)
```

## Error Types

### EnvErr

A sum type representing environment variable lookup failures.

| Variant | Meaning |
|---------|---------|
| NotFound | The environment variable is not set |
| Invalid | The key is empty or contains invalid characters (`=`, NUL) |
