---
title: std.task
slug: task
section: reference/stdlib
order: 65
---

**Status: Implemented, minimal** -- C runtime backing (`src/stdlib/task.c`, pthreads).

The `std.task` module runs zero-argument toke functions on a process-wide thread pool, grouped in *scopes*. You create a scope, spawn functions into it, wait for all of them, then read each result. The pool is started on first use with one worker per online CPU (at most 64).

All handles are `i64`. A function is passed by reference with `&name`; it must take no arguments and return `i64`.

## Limits and behaviour, as implemented

- A scope holds at most 256 tasks; `task.spawn` returns `-1` past that.
- Task handles are numbered from `1` and are **never reused**. At most 4096 tasks can be spawned in the life of the process; after that `task.spawn` still runs the function but returns `-1`, so its result cannot be read.
- `task.awaitall` **busy-waits**: it polls every task's completion flag in a loop with `sched_yield`, so the waiting thread uses a CPU core until the last task finishes.
- Scopes and tasks are never freed.
- Each pool thread has its own error slot (`@tk_current_error` is thread-local, 127.101), so an error raised in one task does not leak into another.

## Functions

### task.scope(): i64

Creates a new scope and returns its handle, or `0` if allocation fails. A scope is an `i64`, so the checker cannot tell it from another integer; the runtime looks every scope argument up among the scopes it created and never reads any other value as one (story 138.47: `task.awaitall(5)` and `task.cancel(5)` died with SIGSEGV before).

### task.spawn(scope: i64; fn: i64): i64

Queues `fn` (written `&name`) to run on the pool and returns a task handle (`1`, `2`, ...). Returns `-1` if `scope` is not a scope `task.scope` returned (`0` included), `fn` is `0`, the scope is full, or the scope has been cancelled.

### task.awaitall(scope: i64): i64

Blocks until every task spawned into `scope` has finished, then returns `0`. Returns `-1` for a value that is not a scope `task.scope` returned (`0` included).

### task.result(handle: i64): i64

Returns the value the task's function returned. Returns `-1` if the handle is unknown or the task has not finished -- so call it after `task.awaitall`, and do not rely on `-1` as a value a task can return.

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
