---
title: std.array
slug: array
section: reference/stdlib
order: 59
---

**Status: Implemented** -- C runtime backing (`src/stdlib/collections_glue.c`, `src/stdlib/str_glue.c` for `array.set`).

The `std.array` module is the module-call spelling of the array built-ins. Most programs never import it: the same operations are available as methods on any array value (`xs.len`, `xs.get(i)`, `xs.append(v)`, `xs.slice(a;b)`, ...), which the guide's Collections lesson covers. Importing `std.array` (for example `i=array:std.array;`) lets you write them as `array.len(xs)`, `array.get(xs;i)` and so on.

Arrays are values. Every function here that "changes" an array returns a **new** array and leaves its argument untouched -- assign the result: `xs=array.push(xs;v);` on a `mut.` binding.

The interface (`stdlib/array.tki`) is generated from the C glue and its entries are tagged `"gen":"abi"`: the arity is checked, but every parameter is declared `i64`. The types in the headings below are the ones the implementation actually reads.

## Where the module form differs from the method form

Two differences, both measured on this tree:

- **Out-of-range `get` does not trap.** `xs.get(i)` traps with `RT003: index <i> out of bounds for length <n>`. `array.get(xs;i)` returns `0` for an index outside `[0, len)`.
- **Search on a `@str` array compares addresses.** `xs.indexof(v)`, `xs.contains(v)` and `xs.find(v)` on a `@str` array compare string content. The module forms `array.indexof`, `array.contains` and `array.find` compare the element words, so on a `@str` array they match only the identical string object -- `array.indexof(str.split("a,b,c";",");"b")` returns `-1`, while the method form returns `1`. Use the method form for string arrays.

## Functions

### array.len(xs: @i64): i64

Returns the number of elements. A null array has length `0`.

### array.get(xs: @i64; idx: i64): i64

Returns the element at `idx`, or `0` when `idx` is outside `[0, len)` (no trap; see above).

### array.push(xs: @i64; val: i64): @i64

Returns a new array: a copy of `xs` with `val` appended.

### array.append(xs: @i64; val: i64): @i64

The same operation as `array.push`.

### array.pop(xs: @i64): @i64

Returns a new array without the last element. It does **not** return the removed element -- read it with `array.get(xs;array.len(xs)-1)` first. Popping an empty array returns the empty array unchanged.

### array.set(xs: @i64; idx: i64; val: i64): @i64

Returns a new array with the element at `idx` replaced by `val`. When `idx` is outside `[0, len)` it returns `xs` itself, unchanged and without an error.

### array.slice(xs: @i64; start: i64; end: i64): @i64

Returns a new array of the elements in the half-open range `[start, end)`. Both bounds are clamped to `[0, len]`, and `start >= end` gives an empty array.

### array.indexof(xs: @i64; val: i64): i64

Returns the index of the first element equal to `val`, or `-1`. Compares element words (see above for `@str` arrays).

### array.find(xs: @i64; val: i64): i64

The same as `array.indexof`: `find` searches by value, not by predicate.

### array.contains(xs: @i64; val: i64): bool

Returns `true` when `array.indexof(xs;val)` is not `-1`.

### array.join(xs: @$str; sep: $str): $str

Joins the elements of a `@str` array with `sep` between them -- the same as `str.join(sep;xs)` with the arguments in array-first order.

## Example

```toke
m=example;
i=io:std.io;
i=array:std.array;

f=main():i64{
  let b=@(3;1;2);
  let c=array.push(b;7);
  let sl=array.slice(c;1;3);
  io.println("len \(array.len(c)) at1 \(array.get(c;1)) idx2 \(array.indexof(c;2))");
  io.println("slice \(array.len(sl)) first \(array.get(sl;0)) b-len \(array.len(b))");
  let j=array.join(@("a";"b");",");
  io.println(j);
  <0
};
```

Prints `len 4 at1 1 idx2 2`, then `slice 2 first 1 b-len 3`, then `a,b`.
