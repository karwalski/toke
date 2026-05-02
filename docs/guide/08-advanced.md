---
title: Lesson 8 — Advanced Topics
slug: 08-advanced
section: learn
order: 8
---

**Estimated time: ~25 minutes**

## Foreign function interface (FFI)

toke compiles to native binaries, which means it can call C functions directly. This is how the standard library is implemented -- toke source calls into C runtime support.

### Extern function declarations

An extern function is declared with `f=` but has no body:

```
f=cstrlen(s:*u8):u64;
```

The missing body tells the compiler this function is defined externally (in C or another language) and will be provided at link time.

### Calling extern functions

Once declared, extern functions are called like any other function:

```
m=ffidemo;

f=cstrlen(s:*u8):u64;
f=cputs(s:*u8):i32;

f=main():i64{
  let msg="Hello from C\0";
  cputs(msg as *u8);
  <0;
};
```

Note the `\0` at the end of the string -- C functions expect null-terminated strings.

### Linking

When using FFI, you tell the compiler which libraries to link:

```bash
tkc ffidemo.tk -lc -o ffidemo
```

The `-lc` flag links the C standard library.

## Pointer types

Pointer types are written `*T` where `T` is the pointed-to type:

| Type | Meaning |
|------|---------|
| `*u8` | Pointer to a byte (used for C strings) |
| `*i32` | Pointer to a 32-bit integer |
| `*void` | Opaque pointer (void pointer) |

### Pointers are FFI-only

In pure toke code, you never need pointers. All memory access is through named fields, array indices, and function calls. Pointers exist solely for interoperability with C libraries.

### Safety warning

Pointer operations bypass toke's safety guarantees. A null pointer dereference through FFI produces runtime trap RT004. Memory accessed through pointers is not arena-managed -- the programmer is responsible for correct lifetime management at the FFI boundary.

## Type casts with `as`

The `as` keyword performs explicit type conversion:

```
let x=42;
let y=x as f64;
let z=3.14 as i64;
let wide=narrow as i64;
```

### Cast rules

- **Numeric widening** (i32 to i64, f32 to f64): always safe
- **Numeric narrowing** (i64 to i32, f64 to f32): compiles with warning W1001, runtime trap if value does not fit
- **Int to float** (i64 to f64): may lose precision for very large integers
- **Float to int** (f64 to i64): truncates toward zero
- **To $str** (`x as $str`): converts numbers to their string representation
- **Pointer casts** (`s as *u8`): for FFI use only

There are **no implicit conversions** in toke. Every conversion must use `as`. This is enforced by the compiler -- passing an `i32` where `i64` is expected is a type error (E4020).

## Arena blocks

:::note
Arena blocks (`arena{...}`) are a planned feature. The syntax is supported by the parser but arena-based allocation is not yet implemented in the compiler backend.
:::

By default, all allocations within a function are freed when the function returns. Arena blocks create shorter-lived allocation regions:

```
f=processlargedata(items:@$str):$str{
  let result=mut."";
  lp(let i=0;i<items.len;i=i+1){
    arena{
      let temp=str.upper(items.get(i));
      let processed=str.replace(temp;" ";"-");
      result=result+processed+"\n";
    };
  };
  <result;
};
```

The `arena{ ... }` block allocates `temp` and `processed` within a sub-arena. When the block exits, those allocations are freed immediately -- not at function exit. The `result` variable, bound outside the arena, survives.

### Why use arena blocks?

In a loop processing large data, allocations can accumulate. Without arena blocks, every temporary string, array, or struct lives until the function returns. Arena blocks let you free temporaries per iteration, keeping memory usage bounded.

### Arena safety

Returning a reference to an arena-allocated value across the arena boundary is a compile error (E5001):

```
arena{
  let temp=@(1;2;3);
  result=temp;
};
```

The compiler prevents use-after-free at the arena boundary.

## Module versioning (future)

Import declarations will support version strings in a future specification:

```
i=http:std.http "1.2";
```