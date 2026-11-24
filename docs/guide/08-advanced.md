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
`{arena ...}` blocks parse and type-check, but they do not release memory. The [memory model](/docs/spec/memory-model/) (§1, amended by story 127.524) describes what ships: values the runtime creates are not freed before the program exits, apart from closure environments and the number conversions inside a string interpolation. Release by `{arena}` is the subject of an ADR (story 127.525).
:::

An arena block marks a region whose temporaries a future release of toke may free when the block exits. Today the block runs exactly like any other block:

```
f=processlargedata(items:@$str):$str{
  let result=mut."";
  lp(let i=0;i<items.len;i=i+1){
    {arena
      let temp=str.upper(items.get(i));
      let processed=str.replace(temp;" ";"-");
      result="\(result)\(processed)\n"
    }
  };
  <result
};
```

`temp` and `processed` are bound inside the block. Nothing is freed when it exits, so `result`, bound outside it, is safe either way.

### Memory in long-running loops

Because nothing is released, every temporary string, array or struct a loop makes stays allocated until the program exits. A short-lived program pays its total allocation once; a server or a long polling loop grows by what each iteration allocates (memory model §1.6). Keep per-iteration allocation small in such loops.

### Arena safety

Inside an `{arena}` block, assigning a string, array, map, struct or closure to a module-level `mut` binding is compile error E5001 (memory model §2.4). Assigning a number or a `bool` is allowed, and so is writing to the function's own outer bindings, as `result` is above. Since the block releases nothing, no program reads freed memory today.

## Module versioning (future)

Import declarations will support version strings in a future specification:

```
i=http:std.http "1.2";
```