---
title: std.queue
slug: queue
section: reference/stdlib
order: 56
---

**Status: Implemented** -- C runtime backing (`src/stdlib/collections.c`, wrappers in `src/stdlib/collections_glue.c`).

The `std.queue` module is a first-in, first-out container of `i64` values. A queue is an opaque `i64` handle returned by `queue.new()`; the functions mutate the queue the handle points at, so the handle does not need to be reassigned. Storage is a circular buffer that starts with room for 16 values and doubles when full.

Queues are never freed: the module has no destroy call, so a queue lives until the process exits.

See also [std.stack](/docs/stdlib/stack/) and [std.set](/docs/stdlib/set/).

## Functions

### queue.new(): i64

Creates an empty queue and returns its handle. Returns `0` if the allocation fails; every other function treats a `0` handle as an empty queue.

### queue.push(q: i64; val: i64): i64

Appends `val` at the back of the queue and returns the same handle `q`.

### queue.pop(q: i64): i64

Removes and returns the value at the front. On an empty queue it returns `0` and leaves the queue unchanged, so check `queue.len(q)` first when `0` is a meaningful value.

### queue.peek(q: i64): i64

Returns the value at the front without removing it, or `0` on an empty queue.

### queue.len(q: i64): i64

Returns the number of values in the queue.

## Example

```toke
m=example;
i=io:std.io;
i=queue:std.queue;

f=main():i64{
  let q=queue.new();
  queue.push(q;10);
  queue.push(q;20);
  let a=queue.pop(q);
  io.println("\(a) then \(queue.peek(q)), \(queue.len(q)) left");
  <0
};
```

Prints `10 then 20, 1 left`.
