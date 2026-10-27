---
title: std.collections
slug: collections
section: reference/stdlib
order: 60
---

**Status: Implemented** -- C runtime backing (`src/stdlib/collections_glue.c`).

The `std.collections` module is three small array helpers. They duplicate the array built-ins and [std.array](/docs/stdlib/array/): nothing here is unavailable elsewhere, and new code should prefer the array methods (`@()`, `xs.append(v)`).

The interface (`stdlib/collections.tki`) is generated from the C glue (`"gen":"abi"`): arity is checked, every parameter is declared `i64`. The types below are the ones the implementation reads.

Despite the module's name, the containers [std.stack](/docs/stdlib/stack/), [std.queue](/docs/stdlib/queue/), [std.set](/docs/stdlib/set/) and [std.vec](/docs/stdlib/vec/) are separate modules with their own imports.

## Functions

### collections.newarray(): @i64

Returns a new empty array -- the same as `@()`.

### collections.append(xs: @i64; item: i64): @i64

Returns a new array: a copy of `xs` with `item` appended. `xs` is unchanged.

### collections.push(xs: @i64; item: i64): @i64

The same operation as `collections.append`.

## Example

```toke
m=example;
i=io:std.io;
i=coll:std.collections;

f=main():i64{
  let n=coll.newarray();
  let m=coll.push(coll.append(n;1);2);
  io.println("\(m.len) \(n.len)");
  <0
};
```

Prints `2 0`: the original empty array is untouched.
