---
title: std.mem
slug: mem
section: reference/stdlib
order: 61
---

**Status: Implemented** -- C runtime backing (`src/stdlib/mem.c`).

The `std.mem` module is raw, unchecked memory access: `malloc`/`free`/`realloc`, `memcpy`/`memset`/`memcmp`, and single-byte load and store. Pointers are plain `u64` values. **Nothing is checked** -- not null pointers, not bounds, not double frees, not use after free. A wrong argument is undefined behaviour in the C sense (typically a crash). Use it for FFI-shaped work where you need a byte buffer at a stable address; ordinary toke code should use arrays, strings and [std.vec](/docs/stdlib/vec/).

Memory from `mem.alloc` / `mem.realloc` is owned by the caller (`"ownership":"caller"` in `stdlib/mem.tki`) and is never reclaimed unless you call `mem.free`.

## Functions

### mem.alloc(size: u64): u64

Allocates `size` bytes (C `malloc`) and returns the address, or `0` if the allocation fails. The contents are uninitialised.

### mem.free(ptr: u64): void

Frees a block returned by `mem.alloc` or `mem.realloc` (C `free`). `mem.free(0)` does nothing.

### mem.realloc(ptr: u64; size: u64): u64

Resizes the block at `ptr` to `size` bytes (C `realloc`) and returns the new address, which may differ from `ptr`. On failure it returns `0` and the original block is still valid.

### mem.copy(dst: u64; src: u64; n: u64): void

Copies `n` bytes from `src` to `dst` (C `memcpy`). The regions must not overlap.

### mem.set(dst: u64; val: u64; n: u64): void

Sets `n` bytes at `dst` to the low byte of `val` (C `memset`).

### mem.cmp(a: u64; b: u64; n: u64): i64

Compares `n` bytes (C `memcmp`): `0` when equal, otherwise negative or positive by the first differing byte.

### mem.load8(ptr: u64; offset: u64): u64

Returns the byte at `ptr + offset`, zero-extended.

### mem.store8(ptr: u64; offset: u64; val: u64): void

Stores the low 8 bits of `val` at `ptr + offset`.

## Example

```toke
m=example;
i=io:std.io;
i=mem:std.mem;

f=main():i64{
  let p=mem.alloc(16);
  mem.set(p;65;16);
  mem.store8(p;1;66);
  let q=mem.alloc(16);
  mem.copy(q;p;16);
  io.println("\(mem.load8(q;0)) \(mem.load8(q;1)) cmp \(mem.cmp(p;q;16))");
  mem.free(p);
  mem.free(q);
  <0
};
```

Prints `65 66 cmp 0`.
