---
title: std.task
slug: task
section: reference/stdlib
order: 65
---

**Status: Implemented, minimal** -- C runtime backing (`src/stdlib/task.c`, pthreads).

The `std.task` module runs zero-argument toke functions on a process-wide thread pool, grouped in *scopes*. You create a scope, spawn functions into it, wait for all of them, then read each result. The pool is started on first use with one worker per online CPU (at most 64).

All handles are `i64`. The function a task runs is a function value that takes no arguments: `&name`, a function name, or a closure (`fn():i64{<k+1}`). Any return type works; `task.result` returns it widened to an `i64` (story 127.549):

| The function returns | `task.result` gives |
|---|---|
| `i64`, `u64` | the value |
| `bool` | `1` or `0` |
| `i8`, `i16`, `i32` / `u8`, `u16`, `u32` | the value, sign- / zero-extended |
| `f64`, `f32` | the bits of the (widened) `f64`; read them back with `math.frombits` |
| `str`, an array, a record | the pointer, as an integer (no toke operation turns it back into the value yet) |

Anything else -- an integer, a function that takes parameters, or a call such as `task.spawn(s;w())`, which would run `w` at once and spawn its result -- is rejected at compile time with E4081.

## Limits and behaviour, as implemented

- A scope holds at most 256 tasks; `task.spawn` returns `-1` past that.
- Task handles are numbered from `1` and are **never reused**. At most 4096 tasks can be spawned in the life of the process; after that `task.spawn` still runs the function but returns `-1`, so its result cannot be read.
- `task.awaitall` **busy-waits**: it polls every task's completion flag in a loop with `sched_yield`, so the waiting thread uses a CPU core until the last task finishes.
- Scopes and tasks are never freed.
- A closure runs with the values it captured. The task holds a reference to the closure's environment from `task.spawn` until the function has returned (or until the task is refused or skipped by `task.cancel`), so the closure stays valid after the spawning function returns; the reference count is atomic.
- Each pool thread has its own error slot (`@tk_current_error` is thread-local, 127.101), so an error raised in one task does not leak into another.

## Functions

### task.scope(): i64

Creates a new scope and returns its handle, or `0` if allocation fails. A scope is an `i64`, so the checker cannot tell it from another integer; the runtime looks every scope argument up among the scopes it created and never reads any other value as one (story 138.47: `task.awaitall(5)` and `task.cancel(5)` died with SIGSEGV before).

### task.spawn(scope: i64; fn: i64): i64

Queues `fn` (a `():T` function value: `&name`, a function name or a closure) to run on the pool and returns a task handle (`1`, `2`, ...). Returns `-1` if `scope` is not a scope `task.scope` returned (`0` included), the scope is full, or the scope has been cancelled. The interface declares `fn` as `i64`, but the compiler checks it: anything other than a function value taking no parameters is E4081.

### task.awaitall(scope: i64): i64

Blocks until every task spawned into `scope` has finished, then returns `0`. Returns `-1` for a value that is not a scope `task.scope` returned (`0` included).

### task.result(handle: i64): i64

Returns the value the task's function returned, widened to `i64` as the table above shows. Returns `-1` if the handle is unknown or the task has not finished -- so call it after `task.awaitall`, and do not rely on `-1` as a value a task can return.

### task.cancel(scope: i64): i64

Marks the scope and every task in it as cancelled and returns `0` (`-1` for a value that is not a scope `task.scope` returned, `0` included). Cancellation does not interrupt a running function: a task that has not *started* yet is skipped and its result is `-1`; one already running completes normally. Later `task.spawn` calls on the scope return `-1`.

## Example

```toke
m=example;
i=io:std.io;
i=task:std.task;

f=work():i64{<42};

f=main():i64{
  let s=task.scope();
  let a=task.spawn(s;&work);
  let b=task.spawn(s;&work);
  let rc=task.awaitall(s);
  io.println("rc \(rc) results \(task.result(a)) \(task.result(b))");
  <0
};
```

Prints `rc 0 results 42 42`.

A `bool`, an `f64` and a closure:

```toke
m=example;
i=io:std.io;
i=task:std.task;
i=math:std.math;

f=ready():bool{<3>0};
f=half():f64{<1.5};

f=main():i64{
  let s=task.scope();
  let k=20;
  let a=task.spawn(s;&ready);
  let b=task.spawn(s;&half);
  let c=task.spawn(s;fn():i64{<k+1});
  let rc=task.awaitall(s);
  io.println("\(task.result(a)) \(math.frombits(task.result(b))) \(task.result(c))");
  <0
};
```

Prints `1 1.5 21`.
