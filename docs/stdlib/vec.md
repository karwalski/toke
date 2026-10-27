---
title: std.vec
slug: vec
section: reference/stdlib
order: 58
---

**Status: Implemented** -- C runtime backing (`src/stdlib/collections.c`, wrappers in `src/stdlib/collections_glue.c`).

The `std.vec` module is a mutable, growable vector of `i64` values. toke arrays (`@(...)`) are values: `array.push` and `array.set` return a new array. A `$vechandle` is instead a handle to one shared buffer, so `vec.push` appends in amortised O(1) and `vec.set` replaces an element in place in O(1). Use it for algorithms that build or update large arrays element by element, and convert to and from an ordinary array with `vec.tovec` / `vec.toarray`.

`vec.push` and `vec.set` return the handle they were given; the examples assign it back (`v=vec.push(v;x);` on a `mut.` binding), as the conformance tests do.

## Bounds

An index outside `[0, len)` passed to `vec.get` or `vec.set` -- in the module form `vec.get(v;i)` or the receiver forms `v.get(i)` / `v.set(i;x)` -- traps at run time with `RT003: index <i> out of bounds for length <n>` and exit status 1. The compiler emits the check before the call.

`vec.pop` on an empty vector returns `0` and does not trap.

## Types

### $vechandle

An opaque handle to a vector. It has no fields. Vectors are never freed: the module has no destroy call.

## Functions

### vec.new(): $vechandle

Creates an empty vector (initial capacity 16).

### vec.push(v: $vechandle; val: i64): $vechandle

Appends `val` and returns `v`.

### vec.pop(v: $vechandle): i64

Removes and returns the last element, or `0` if the vector is empty.

### vec.get(v: $vechandle; idx: i64): i64

Returns the element at `idx`. Traps `RT003` when `idx` is out of range (see Bounds).

### vec.set(v: $vechandle; idx: i64; val: i64): $vechandle

Replaces the element at `idx` with `val`, in place, and returns `v`. Traps `RT003` when `idx` is out of range.

### vec.len(v: $vechandle): i64

Returns the number of elements.

### vec.tovec(arr: @i64): $vechandle

Returns a new vector holding a copy of the elements of `arr`.

### vec.toarray(v: $vechandle): @i64

Returns a new array holding a copy of the vector's current elements. Later changes to the vector do not affect it.

## Example

```toke
m=example;
i=io:std.io;
i=vec:std.vec;

f=main():i64{
  let v=mut.vec.new();
  v=vec.push(v;4);
  v=vec.push(v;5);
  v=vec.set(v;0;9);
  io.println("\(vec.get(v;0)) \(vec.get(v;1)) len \(vec.len(v))");
  let a=vec.toarray(vec.tovec(@(1;2;3)));
  io.println("array of \(a.len), last \(a.get(2))");
  <0
};
```

Prints `9 5 len 2`, then `array of 3, last 3`.
