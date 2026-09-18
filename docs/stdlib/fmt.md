---
title: std.fmt
slug: fmt
section: reference/stdlib
order: 42
---

**Status: Implemented** -- C runtime backing (story 131.30).

The `std.fmt` module renders values as text for printing. It exists so that programs never need a hand-written `show`/`boolstr` helper to print a bool, an array, or a float with a fixed number of decimals. Every function returns a fresh string; none of them writes to stdout -- pair them with `io.println`.

Module style only: import as `i=fmt:std.fmt;` and call `fmt.bool(...)`, `fmt.arr(...)`, and so on. Call names contain no underscores.

## Functions

### fmt.bool(b: bool): $str

Returns `"true"` or `"false"`.

### fmt.arr(xs: @i64; sep: $str): $str

Joins the decimal rendering of each element of `xs` with `sep`. An empty array yields `""`; a single element yields that element with no separator.

### fmt.strs(xs: @str; sep: $str): $str

Joins the elements of `xs` with `sep`. An empty array yields `""`.

### fmt.f64(x: f64; prec: i64): $str

Renders `x` with exactly `prec` digits after the decimal point (`prec` 0 gives an integer-looking string, no trailing point). Negative `prec` is treated as 0; values above 20 are clamped to 20. Rounding follows the C library's `%.*f`, i.e. round-half-to-even on the binary value: `fmt.f64(2.675;2)` is `"2.67"`.

### fmt.pad(s: $str; width: i64; left: bool): $str

Pads `s` with spaces to `width` code points. `left=true` puts the padding before `s` (right-aligns the text); `left=false` puts it after. When `s` is already `width` or wider it is returned unchanged.

## Usage Example

```toke
m=example;
i=io:std.io;
i=fmt:std.fmt;

f=main():i64{
  let xs=@(3;1;2);
  io.println(fmt.arr(xs;", "));
  io.println(fmt.bool(xs.len>2));
  io.println(fmt.f64(2.0/3.0;3));
  io.println(fmt.strs(@("a";"b");"-"));
  io.println(fmt.pad("id";6;false));
  <0
};
```

Prints:

```text
3, 1, 2
true
0.667
a-b
id    
```

## See Also

- [std.str](/docs/stdlib/str) -- general string manipulation (`s.join`, `s.fromint`)
- [std.io](/docs/stdlib/io) -- `io.println`
