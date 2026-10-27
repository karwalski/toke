---
title: std.stack
slug: stack
section: reference/stdlib
order: 55
---

**Status: Implemented** -- C runtime backing (`src/stdlib/collections.c`, wrappers in `src/stdlib/collections_glue.c`).

The `std.stack` module is a last-in, first-out container of `i64` values. A stack is an opaque `i64` handle returned by `stack.new()`; the functions mutate the stack the handle points at, so the handle does not need to be reassigned after a push or pop. Storage is a dynamic array that starts with room for 16 values and doubles when full.

Stacks are never freed: the module has no destroy call, so a stack lives until the process exits.

See also [std.queue](/docs/stdlib/queue/), [std.set](/docs/stdlib/set/) and [std.vec](/docs/stdlib/vec/), which share the same implementation file.

## Functions

### stack.new(): i64

Creates an empty stack and returns its handle. Returns `0` if the allocation fails; every other function treats a `0` handle as an empty stack.

### stack.push(s: i64; val: i64): i64

Pushes `val` on top of the stack and returns the same handle `s`.

### stack.pop(s: i64): i64

Removes and returns the top value. On an empty stack it returns `0` and leaves the stack unchanged -- so a stored `0` and an empty stack are indistinguishable; check `stack.empty(s)` or `stack.len(s)` first.

### stack.peek(s: i64): i64

Returns the top value without removing it, or `0` on an empty stack.

### stack.len(s: i64): i64

Returns the number of values on the stack.

### stack.empty(s: i64): i64

Returns `1` if the stack holds no values, `0` otherwise. The result is an `i64`, not a `bool`.

## Example

```toke
m=example;
i=io:std.io;
i=stack:std.stack;

f=main():i64{
  let s=stack.new();
  stack.push(s;1);
  stack.push(s;2);
  io.println("top \(stack.peek(s)) of \(stack.len(s))");
  let a=stack.pop(s);
  let b=stack.pop(s);
  io.println("\(a) \(b) empty=\(stack.empty(s))");
  <0
};
```

Prints `top 2 of 2`, then `2 1 empty=1`.
