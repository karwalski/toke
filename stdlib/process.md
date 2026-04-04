# std.process -- Process Management

## Overview

The `std.process` module provides functions for spawning child processes, reading their output, waiting for completion, and sending signals. Process handles are opaque values obtained from `process.spawn`.

## Types

### Handle

An opaque handle representing a running child process. Obtained from `process.spawn` and passed to `process.wait`, `process.stdout`, and `process.kill`.

## Functions

### process.spawn(cmd: [Str]): Handle!ProcessErr

Spawns a child process. The first element of `cmd` is the executable path or name; subsequent elements are arguments. Returns a `Handle` on success, or `ProcessErr.NotFound` if the executable cannot be found, `ProcessErr.Permission` if execution is denied.

**Example:**
```toke
let h = process.spawn(["echo"; "hello toke"]);
(* h = ok(Handle{...}) *)

let e = process.spawn(["/nonexistent_binary"]);
(* e = err(ProcessErr.NotFound{...}) *)
```

### process.wait(h: Handle): i32!ProcessErr

Blocks until the child process exits and returns its exit code. Safe to call multiple times on the same handle (returns the cached exit code on subsequent calls). Returns `ProcessErr.IO` if waiting fails.

**Example:**
```toke
let code = process.wait(h);  (* code = ok(0) *)
```

### process.stdout(h: Handle): Str!ProcessErr

Reads all stdout output from the child process. Drains the pipe on the first call; subsequent calls return an empty string. Returns `ProcessErr.IO` if the read fails.

**Example:**
```toke
let h = process.spawn(["echo"; "hello toke"]);
let out = process.stdout(h);  (* out = ok("hello toke\n") *)
```

### process.kill(h: Handle): bool

Sends SIGTERM to the child process. Returns `true` if the signal was sent successfully, `false` if the process could not be signalled (e.g., already exited or null handle). This function is infallible.

**Example:**
```toke
let h = process.spawn(["sleep"; "60"]);
let ok = process.kill(h);   (* ok = true *)
process.wait(h);             (* reap the process *)
```

## Error Types

### ProcessErr

A sum type representing process operation failures.

| Variant | Meaning |
|---------|---------|
| NotFound | The executable was not found |
| Permission | Permission denied when attempting to execute |
| IO | An I/O error occurred during pipe read, write, or wait |
