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

**Only a value `process.spawn` returned is a handle.** The runtime keeps a registry of the handles it has handed out and looks every argument up there without reading it as memory. Any other value (an integer, a pid from `process.spawndetached`, a handle changed by arithmetic, a value cast with `as`) is answered exactly as `process.badhandle()` (`0`) is: `process.wait` and `process.exitcode` return `-1`, `process.stdout` and `process.stderr` take the `$err` arm with `$io`, `process.kill` returns `false`, `process.isalive` returns `0`, and `process.hasexited` and `process.poll` return `1`. Before 138.36 such a value was read as a process record, and the program died with SIGSEGV (loke T-67).

A waited handle stays valid: `process.wait` returns the same exit code again, `process.stdout` and `process.stderr` return the output drained while waiting, and `process.kill` returns `false` because the child has exited. No call in `std.process` frees a handle. Once the child has been reaped (by `process.wait`, or by a `process.poll`, `process.hasexited` or `process.isalive` that sees it exit), the runtime closes the child's stdin pipe and reads what is already in its output pipes into the handle, so a reaped handle holds no file descriptor (unless a grandchild still holds an output pipe open) and a long-running program can spawn without limit; before 138.48 every spawn kept its stdin pipe open for the life of the program.

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

### process.exit(code: i64): i64

Ends the process at once with `code`. It is `os.exit` under another name and
follows the same rule (127.532): \_exit(2), so `atexit` handlers do not run and
buffered stdout is **not** flushed, and `process.exit(0)` after a recorded
failure -- a failed `test.eq`, `test.ok` or `test.assert*`, or an
`http.servetls` that could not start -- prints
`DIAGNOSTIC: kind=exit_status status=1 ...` and exits `1` instead. A non-zero
`code` is kept as given. Under `TK_MEM_STATS=1` the allocation report is
printed once before the process ends. Does not return. Before 127.532 (and on
v3.2.0) `process.exit(0)` exited `0` after a failure while `os.exit(0)` exited
`1`.

The other ways a toke program ends, and whether a recorded failure survives
them:

| Exit path | Status after a recorded failure |
|---|---|
| return from `main` with `0` | `1` (the at-exit handler, 127.231) |
| return from `main` with a non-zero status | that status (135.29) |
| `os.exit` or `process.exit` with status 0 | `1` |
| `os.exit` or `process.exit` with a non-zero status `n` | `n` as \_exit(2) truncates it; see the note below |
| a runtime trap through `exit(1)` (`RT002`-`RT010`: overflow, division, bounds, nil, map key, re-raise; a capability refusal; `test.report` with failures; most out-of-memory reports) | `1` |
| `abort()` or a signal | killed by the signal; no `exit_status` line |
| the child of `process.spawn` between `fork` and `exec` (`_exit(1)`), and each `http` worker process (`_exit(0)` when its loop ends) | not the program's status: these are separate processes, and a failure recorded inside a worker is not passed to the parent |

A non-zero status whose low 8 bits are zero (`256`, `512`, ...) is truncated to
`0` by the operating system, so `os.exit(256)` or `process.exit(256)` after a
failure exits `0`, while `main` returning `256` exits `1` (tracked as 127.536).

<!-- skip-check -->
```toke
m=main;
i=test:std.test;
i=process:std.process;

f=main():i64{
  test.eq(1;2);
  process.exit(0);
  <0
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
