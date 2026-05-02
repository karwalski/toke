# Arena-Aware Return Value Copying — Design Note

Story: 76.1.4b

## Status

Current codegen is correct.  No functional changes required for the
alloca + C-heap memory model in use today.  This document records the
analysis and describes what would change if toke adopts real per-function
arenas.

## Current Memory Model

toke's LLVM codegen uses two allocation strategies:

| Category | Mechanism | Lifetime |
|----------|-----------|----------|
| Scalar locals (i64, i1, f64, f32) | `alloca` in function entry block | Freed automatically on function return |
| Struct locals | `alloca %struct.T` in function entry block | Freed automatically on function return |
| Strings from stdlib (str.concat, str.trim, ...) | `malloc`/`strdup` inside C runtime wrappers | Heap — survives until explicit `free` or program exit |
| Arrays from stdlib | `malloc` inside C runtime wrappers | Heap — survives until explicit `free` or program exit |
| String literals | Module-level `@.str.N` globals | Static — lives for the entire program |

## Why Return Values Are Safe Today

### Scalars (i64, i1, double, float)

LLVM's `ret <ty> %tN` instruction copies the value into the caller's
SSA register before the callee's stack frame is deallocated.  This is
fundamental to SSA — the value exists in a virtual register, not a
memory location.

### Strings and Arrays (heap-allocated)

When a function calls `tk_str_concat_w(a, b)` and returns the result,
the returned i64 is a pointer (cast to integer by the wrapper ABI) to
heap memory allocated by the C runtime.  The callee's stack frame has
no ownership of this memory, so returning from the callee does not
invalidate it.

The caller receives the i64 value in an SSA register and can use it
freely.  The heap memory remains valid until it is explicitly freed
(which toke currently never does — memory is reclaimed at process exit).

### Struct Values (stack-allocated, pointer-returned)

Structs are `alloca`'d on the callee's stack and returned as an `i8*`
(pointer to the alloca).  This is the one case where the returned value
*could* become a dangling pointer after the callee returns.

In practice this is safe today because:

1. The caller immediately dereferences the returned pointer to extract
   field values (via GEP + load), which happens before any other call
   can reuse the callee's stack memory.

2. LLVM's optimiser typically promotes small struct allocas to registers
   anyway (mem2reg / SROA), eliminating the pointer entirely.

**This is the category that would need explicit copying if toke
implements per-function arenas.**

## Future: Per-Function Arena Migration

If toke replaces `alloca` with a per-function arena allocator (e.g., a
bump allocator reset on function return), the following changes would be
required in the LLVM codegen:

### 1. Arena-Allocated Structs Must Be Copied Before Return

When a function returns a struct pointer, the codegen would need to:

```
; Before (current — alloca on stack):
  %result = alloca %struct.Point
  ; ... fill fields ...
  %ptr = bitcast %struct.Point* %result to i8*
  ret i8* %ptr

; After (arena model — copy to caller arena):
  %result = call i8* @arena_alloc(%caller_arena, i64 <struct_size>)
  ; ... fill fields into %result ...
  ret i8* %result
```

The key change: the allocation must come from the *caller's* arena (or a
parent arena), not the callee's arena, so it survives the callee's arena
reset.

### 2. Arena Handle Threading

Each function would receive an implicit arena parameter (or use a
thread-local arena stack).  The codegen would emit:

- `arena_push()` at function entry (create a child arena)
- `arena_pop()` at function exit (free all callee-local allocations)
- Return values allocated from the *parent* arena

### 3. String/Array Returns

If strings and arrays move from C-heap to arena allocation, the same
copy-to-caller-arena rule applies.  The stdlib wrappers would need to
accept an arena parameter:

```c
// Current:
i64 tk_str_concat_w(i64 a, i64 b);  // mallocs internally

// Future:
i64 tk_str_concat_w(i8* arena, i64 a, i64 b);  // allocates from arena
```

### 4. NODE_ARENA_STMT Integration

The existing `NODE_ARENA_STMT` (currently a comment-only placeholder)
would become the explicit arena scope boundary.  Code inside an `arena`
block would use a child arena; values escaping the block would need
copying.

## Verification Checklist

- [x] Scalar returns: safe (value semantics via SSA registers)
- [x] String returns from stdlib: safe (C-heap allocated, not stack)
- [x] String literal returns: safe (module-level globals, static lifetime)
- [x] Array returns from stdlib: safe (C-heap allocated, not stack)
- [x] Struct returns: safe under current usage patterns (immediate field extraction)
- [ ] Struct returns with deferred field access: would be unsafe with real arenas (future work)

## References

- `src/llvm.c` NODE_RETURN_STMT handler (line ~3094)
- `src/llvm.c` NODE_ARENA_STMT placeholder (line ~3337)
- `src/llvm.c` emit_toplevel NODE_FUNC_DECL implicit return (line ~3558)
- Story 76.1.4b (formal memory model — arena-aware return copying)
