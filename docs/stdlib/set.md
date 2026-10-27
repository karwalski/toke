---
title: std.set
slug: set
section: reference/stdlib
order: 57
---

**Status: Implemented** -- C runtime backing (`src/stdlib/collections.c`, wrappers in `src/stdlib/collections_glue.c`).

The `std.set` module holds unique `i64` values. A set is an opaque `i64` handle returned by `set.new()`; the functions mutate the set the handle points at. Storage is a sorted array searched by binary search, so `set.has` is O(log n) and `set.add` / `set.remove` are O(n) (elements are shifted to keep the array sorted).

Values are compared as integers.

Sets are never freed: the module has no destroy call. There is no iteration call; a set answers membership and size only.

## Functions

### set.new(): i64

Creates an empty set and returns its handle, or `0` if the allocation fails. Every other function treats a `0` handle as an empty set.

### set.add(s: i64; val: i64): i64

Adds `val` if it is not already a member and returns the same handle `s`. Adding a value that is already present does nothing.

### set.has(s: i64; val: i64): i64

Returns `1` if `val` is a member, `0` otherwise. The result is an `i64`, not a `bool`.

### set.remove(s: i64; val: i64): i64

Removes `val` if it is a member and returns the same handle `s`. Removing a value that is not present does nothing.

### set.len(s: i64): i64

Returns the number of members.

## Example

```toke
m=example;
i=io:std.io;
i=set:std.set;

f=main():i64{
  let t=set.new();
  set.add(t;5);
  set.add(t;3);
  set.add(t;5);
  io.println("len \(set.len(t)) has5 \(set.has(t;5)) has9 \(set.has(t;9))");
  set.remove(t;5);
  io.println("len \(set.len(t)) has5 \(set.has(t;5))");
  <0
};
```

Prints `len 2 has5 1 has9 0`, then `len 1 has5 0`.
