---
title: Memory Model Specification
slug: memory-model
section: spec
order: 6
---

# toke Memory Model Specification

**Status:** Normative [N]
**Extends:** toke Language Specification v0.3, Section 14
**Version:** 0.3.2

This document formalises the memory model that toke programs operate under. It covers arena allocation, value lifetimes, concurrency memory semantics, and FFI boundary rules. All sections are normative unless marked [I].

---

## 1. Arena Allocation Model [N]

### 1.1 Per-Scope Arenas

Every function body implicitly creates an arena. All allocations made during the execution of that function (strings, arrays, maps, structs) are placed in the function's arena.

The arena uses a bump allocator: allocation advances a pointer and returns the previous position. This gives O(1) allocation cost with no per-object overhead.

### 1.2 Sub-Arenas

Explicit `{arena ... }` blocks create sub-arenas with shorter lifetimes. A sub-arena is a child of its enclosing arena. All allocations within the block go to the sub-arena.

### 1.3 Bulk Deallocation

When a scope exits (function return, arena block exit), the entire arena is freed in a single operation. There are no individual `free()` calls. This eliminates:

- **Use-after-free**: memory is only freed when the entire arena is discarded, and no references to the arena can exist after scope exit.
- **Double-free**: there is no mechanism to free an individual allocation, so double-free is structurally impossible.

### 1.4 String Literals

String literals are statically allocated in the binary's read-only data segment. They are not placed in any arena and have program lifetime. Concatenation and other string operations that produce new strings allocate in the current scope's arena.

### 1.5 No Individual Deallocation

toke provides no mechanism to free a single allocation within an arena. The only deallocation operation is bulk arena release on scope exit. This is a deliberate design constraint, not an omission.

---

## 2. Lifetime Rules [N]

### 2.1 Scope Lifetime

A value allocated in a scope lives until the scope exits. Within the scope, the value is always valid and accessible.

### 2.2 Passing Values Down (Arguments)

Passing a value as a function argument is always safe. The caller's arena outlives the callee's arena, so the callee can read the value for the duration of the call. The callee must not store references to argument memory in locations that outlive the call.

### 2.3 Passing Values Up (Return)

When a function returns a value, the value is copied into the caller's arena. This extends its effective lifetime to the caller's scope. The original allocation in the callee's arena is freed when the callee's arena is released, but the returned copy persists in the caller's arena.

A conforming implementation shall ensure that return values are available in the caller's arena before the callee's arena is freed.

### 2.4 Escape Analysis

A value allocated in scope A shall not be stored in a location with a lifetime longer than A, except via the return mechanism described in 2.3. Violation is compile error E5001.

The compiler performs escape analysis at compile time. If a value would outlive its arena through any path other than return, the program is rejected.

### 2.5 Closure Capture

Closures capture values by copy. When a closure is created, the values it references are copied into the closure's environment struct. The environment struct is allocated with `malloc` (not in an arena) so that the closure can outlive the scope in which it was created.

The closure's environment struct is freed when the last reference to the closure is released. This is the sole use of reference-tracked allocation in toke and is managed internally by the runtime, not exposed to user code.

### 2.6 Static Lifetime

Module-level constants and declarations have static lifetime. They are initialised once at program start in declaration order and are never freed during execution. Circular initialisation dependencies are compile error E2020.

---

## 3. Concurrency and Memory [N]

### 3.1 Per-Task Arenas

Each spawned task (via `sc` structured concurrency) receives its own arena, independent of the spawning task's arena. Tasks do not share arena memory.

### 3.2 No Shared Mutable State

Values shared between tasks must be copied at the point of task creation. There is no mechanism to share mutable memory between tasks. This eliminates data races by construction.

When a task is spawned, any values the task body references are copied into the new task's arena. The spawning task and the spawned task then operate on independent copies.

### 3.3 Structured Concurrency Lifetime Guarantee

The `sc` (structured concurrency) construct guarantees that a parent task's arena outlives all child task arenas. A parent task does not exit its `sc` block until all child tasks have completed and their arenas have been freed.

This provides a strict nesting property: child arena lifetimes are always contained within parent arena lifetimes.

### 3.4 Task Return Values

When a child task produces a result, the result is copied into the parent task's arena, following the same rules as function return values (Section 2.3).

---

## 4. FFI Boundary [N]

### 4.1 Pointers Passed to C

When toke calls a C function via FFI, arena-allocated values are passed as raw pointers to the underlying memory. The C function receives a pointer that is valid for the duration of the call.

### 4.2 C Must Not Free Arena Memory

C code must not call `free()` on pointers that originate from toke arenas. Arena memory is managed exclusively by the toke runtime. Calling `free()` on arena memory is undefined behaviour.

### 4.3 C-Allocated Memory

Memory allocated by C code (via `malloc`, `calloc`, etc.) is the responsibility of the C code or the toke caller, depending on the documented ownership convention. The toke runtime does not track or free C-allocated memory.

### 4.4 Ownership Annotations

The `.tki` interface file format (see Section 15.5 of the language specification) supports ownership annotations for FFI declarations. These annotations document which side (toke or C) owns memory passed across the boundary:

- **`@owned`**: the callee takes ownership and is responsible for freeing.
- **`@borrowed`**: the callee borrows the memory; the caller retains ownership.
- **`@caller-frees`**: the callee allocates; the caller must free.

These annotations are documentation and are checked by linting tools. The compiler does not enforce FFI ownership at runtime.

### 4.5 FFI Safety Boundary

The memory safety guarantees in Section 6 apply only to pure toke code. Any function that calls into C via FFI is outside the safety boundary. Bugs in C code (use-after-free, buffer overflow, etc.) can violate toke's memory model. The compiler emits diagnostic W5010 when FFI calls are present, noting the safety boundary.

---

## 5. What toke Does NOT Have [I]

This section clarifies memory management features that toke deliberately omits.

| Feature | Status | Rationale |
|---|---|---|
| Garbage collector | Not present | Arena bulk-free is simpler, deterministic, and has no pause times. |
| Reference counting | Not present | Arenas eliminate the need for per-object lifetime tracking. Closures are the sole exception (Section 2.5), managed internally. |
| Borrow checker | Not present | Values are always copied or arena-allocated. Ownership is structural (arena scope), not tracked per-reference. |
| Manual malloc/free | Not present in toke code | Only accessible through C FFI. toke code uses arenas exclusively. |
| Weak references | Not present | No reference system exists to have a weak variant of. |
| Finalizers/destructors | Not present | Arena bulk-free does not run per-object cleanup. Resources requiring cleanup (file handles, connections) use explicit close calls. |

---

## 6. Formal Properties [N]

A conforming toke implementation shall guarantee the following properties for well-typed toke code that does not use FFI:

### 6.1 No Use-After-Free

Arena memory is freed only by bulk arena release. No reference to arena memory can exist after the arena is released, because the arena's lifetime is the enclosing scope, and all references are scoped to that scope or shorter.

**Proof sketch:** All allocations are in an arena tied to a lexical scope. All references to those allocations are in the same scope or a nested scope. When the scope exits, all references go out of scope simultaneously with the arena release. Therefore no reference can be used after its arena is freed.

### 6.2 No Double-Free

There is no operation to free an individual allocation. The only deallocation is bulk arena release, which occurs exactly once per scope exit. Therefore no allocation can be freed more than once.

### 6.3 No Dangling Pointers Within a Scope

Within a scope, all arena-allocated values remain valid because the arena persists for the scope's entire duration. Combined with escape analysis (Section 2.4), no pointer can refer to memory from a shorter-lived scope.

### 6.4 Bounded Memory Leaks

Memory allocated in an arena is freed when the scope exits. The maximum unreclaimable memory at any point is bounded by the sum of all currently-live arena sizes. Long-running loops that allocate should use sub-arenas (Section 1.2) to bound per-iteration allocation.

### 6.5 No Data Races

Tasks do not share mutable memory (Section 3.2). All inter-task communication is by copy. Therefore concurrent access to the same mutable memory location cannot occur.

---

## 7. Implementation Notes [I]

### 7.1 Arena Sizing

A conforming implementation may use any strategy for arena sizing (fixed, growing, linked-list of pages). The specification does not prescribe a particular approach. The only requirement is that allocation within an arena is O(1) amortised.

### 7.2 Return Value Optimisation

An implementation may optimise return-value copying (Section 2.3) by allocating the return value directly in the caller's arena when this can be determined statically. This is an optimisation; the observable behaviour must be as if the copy occurred.

### 7.3 Large Allocation Fallback

An implementation may fall back to a separate allocation for objects that exceed the arena page size, provided the allocation is still freed when the arena is released.

---

## References

- toke Language Specification v0.3, Section 14 — Memory Model
- toke Language Specification v0.3, Section 15.5 — Interface Files
- toke Language Specification v0.3, Section 24.2 — FFI (deferred)
