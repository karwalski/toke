---
title: std.process
slug: process
section: reference/stdlib
order: 30
---

**Status: Implemented** -- C runtime backing.

The `std.process` module provides functions for spawning child processes, reading their output, waiting for completion, and sending signals. Process handles are opaque values obtained from `process.spawn` and must be passed to all subsequent process functions.

> **Note:** `process.tki` exports four functions: `process.spawn`, `process.wait`, `process.stdout`, and `process.kill`. Additional functions (`process.stderr`, `process.stdin_write`, `process.exit_code`, `process.is_running`, `process.timeout`) are planned but not yet in the current tki -- do not use them.

**The home directory.** `process.homedir()` returns `$HOME` (or `/tmp` when it is unset) as a `$str`. The same value is published at top level as `std.homedir()` (`i=std:std;`, interface `stdlib/std.tki`). Before 138.11 `std.homedir` had no interface, so it interpolated as a number (the string's address), and it linked only when `std.process` was imported as well; the `std` module now declares it and pulls in `std.process` itself.

## Types

### $handle

An opaque handle representing a spawned child process. Obtained from `process.spawn` and passed to `process.wait`, `process.stdout`, and `process.kill`.

### $processerr

A sum type representing process operation failures.

| Variant | Meaning |
|---------|---------|
| $notfound | The executable was not found on the path or at the given location |
| $permission | Permission was denied when attempting to execute the file |
| $io | An I/O error occurred during pipe read, fork, or wait |

## Functions

### process.spawn(cmd: @($str)): $handle!$processerr

Forks and execs the child process, where `cmd.get(0)` is the executable path or name and the remaining elements are arguments. Returns a `$handle` on success; returns `$processerr.$notfound` if the executable cannot be located.

<!-- skip-check -->
```toke
m=main;
i=process:std.process;

f=demo():void{
  let h=mt process.spawn(@("echo";"hello toke")) {$ok:v v;$err:e process.badhandle()};
};
```

### process.wait(h: $handle): i32

Blocks until the child process exits and returns its integer exit code, or `-1` if the handle is invalid or the underlying wait syscall fails. It is a plain `i32`, not an error union — compare the code; do not `mt` it with `$ok`/`$err` arms (that is E4031, 127.202: a `$ok`/`$err` match on an `i32` takes `$err` whenever the code is 0, i.e. on success).

<!-- skip-check -->
```toke
m=main;
i=process:std.process;

f=demo():void{
  let h=mt process.spawn(@("echo";"hello toke")) {$ok:v v;$err:e process.badhandle()};
  let code=process.wait(h);
};
```

### process.stdout(h: $handle): $str!$processerr

Reads and returns all pending stdout output from the child process as a UTF-8 string. Drains the pipe on the first call; subsequent calls return an empty string. Returns `$processerr.$io` if the pipe read fails.

<!-- skip-check -->
```toke
m=main;
i=process:std.process;

f=demo():void{
  let h=mt process.spawn(@("echo";"hello toke")) {$ok:v v;$err:e process.badhandle()};
  let out=mt process.stdout(h) {$ok:s s;$err:e ""};
};
```

### process.kill(h: $handle): bool

Sends SIGTERM to the child process and returns `true` if the signal was delivered successfully, or `false` if the process could not be signalled (e.g. it has already exited). Always reap the process with `process.wait` after calling `kill`.

<!-- skip-check -->
```toke
m=main;
i=process:std.process;

f=demo():void{
  let h=mt process.spawn(@("sleep";"60")) {$ok:v v;$err:e process.badhandle()};
  let ok=process.kill(h);
  let code=process.wait(h);
};
```

## Usage Examples

The following example shows a spawn-and-read-stdout pattern. A child `echo` process writes to stdout; the example reads the output and waits for the process to exit.

```toke
m=main;
i=process:std.process;
i=log:std.log;
i=str:std.str;

f=main():i64{
  let h=mt process.spawn(@("echo";"round trip")) {$ok:v v;$err:e process.badhandle()};
  let out=mt process.stdout(h) {$ok:s s;$err:e ""};
  let code=process.wait(h);
  let msg=str.concat("exit: ";str.fromint(code as i64));
  log.info(msg;@());
  <0;
};
```

The following example shows how to kill a long-running process and wait for it to be reaped.

```toke
m=main;
i=process:std.process;
i=log:std.log;

f=main():i64{
  let h=mt process.spawn(@("sleep";"30")) {$ok:v v;$err:e process.badhandle()};
  let killed=process.kill(h);
  let code=process.wait(h);
  log.info("process cleaned up";@());
  <0;
};
```
