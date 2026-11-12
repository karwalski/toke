---
title: std.option
slug: option
section: reference/stdlib
order: 66
---

**Status: Implemented** -- a convention, not a runtime library. `$none` is built into the compiler; `stdlib/option.tki` declares it and has no C backing.

The `std.option` module names toke's convention for a value that may be absent. An option type is an error union `T!$none`, where `$none` is a built-in variant with no fields that means "no value". It reuses the error-union machinery (`T!$err`): there is no separate option type, no new syntax, and nothing to link.

A program does not need to import `std.option` to use `$none`. `i=o:std.option;` is accepted and adds nothing.

## Types

### $none

A variant with no fields. Construct it as `$none{}`.

## The convention

A function that may return a `T` or nothing declares `T!$none`. It returns the value directly for the present case and `$none{}` for the absent one. The caller matches with `mt`, using an `$ok` arm for the value and a `$none` arm for its absence:

```toke
m=optiondemo;
i=io:std.io;
i=s:std.str;
f=find(h:$str;n:$str):i64!$none{
  let i=s.indexof(h;n);
  if(i<0){<$none{}};
  <i
};
f=main():i64{
  let a=mt find("hello";"l"){$ok:v v;$none:z 0-1};
  let b=mt find("hello";"x"){$ok:v v;$none:z 0-1};
  io.println("a=\(a) b=\(b)");
  <0
};
```

This prints `a=2 b=-1`.

Every arm binds a name, even the `$none` arm, which has nothing in it to read. The name must start with a lower-case letter: `_` is rejected with E1003, because underscores are outside toke's character set. The value type can be anything, for example `$str!$none`.

Because an option is an ordinary error union, the absent arm can also be written `$err:z`, and the match behaves the same. Write `$none:z` when the function's error type is `$none`, so the reader can see the absence is expected and not a failure.

## Notes

- The examples inside `stdlib/option.tki` itself are written in pre-v0.4 syntax (`F=`, `expr|{...}` and `$none:_`) and do not compile as written. The program above is the current form; `make check-docs` compiles and links it.
- An option says only that a value is absent. When the caller needs to know *why*, declare an error sum type instead, as `std.http` does with `$httperr`.
