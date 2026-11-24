---
title: toke Memory Model Specification
slug: memory-model
section: spec
order: 6
---

**Status:** Normative [N]
**Extends:** toke Language Specification §14 (v0.3 base as amended by toke-spec-v0.4.md)
**Version:** 0.4, amended 2026-11-24 (story 127.524)

> **Amended 2026-11-24 (story 127.524, owner answer 1.13 (b), `docs/owner-decisions-2026-11-12.md`, revised 2026-11-13).**
> Until this amendment, §1.1 said that every function body allocates in an arena, §1.3 that the arena is freed in one operation when the scope exits, and §2.3 that a returned value is copied into the caller's arena. No toke implementation has ever done this. This document now describes the allocation model that ships: runtime-created values come from `malloc` or from the S1 sealed-string heap and are not released before the program exits, with the exceptions listed in §1.3. The per-function arena is **not implemented** and is no longer part of the model. Releasing memory before exit, starting with the explicit `{arena}` block, is the subject of an ADR under story 127.525. Until that ADR is accepted, `{arena}` releases nothing (§1.2). The design input for both stories is `docs/decisions/127.299-function-arena.md`.

This document formalises the memory model that toke programs operate under. It covers allocation, value lifetimes, concurrency memory semantics, and FFI boundary rules. All sections are normative unless marked [I].

---

## 1. Allocation Model [N]

### 1.1 Where Values Are Allocated

A toke program does not allocate in per-function or per-scope arenas. Every value that the runtime creates while the program runs (a string, array, map, struct or sum box) is allocated from one of two process-wide sources:

- **The S1 sealed-string heap.** String producers that have been converted to it (concatenation, interpolation, `slice`, `trim`, `split`, `file.read` and others; the census is in `docs/runtime-abi.md` §3.1) allocate from a single reserved region by an atomic bump of its top pointer. A sealed string is never written, freed or reallocated after it is sealed.
- **`malloc`.** Arrays (including `[byte]`), maps, struct and sum boxes, and the string producers that have not been converted to the sealed heap (for example `str.fromint`, `str.fromfloat` and strings made by `std.json`) are allocated with `malloc`.

An error value is boxed in a per-thread buffer that is reused (127.109). A string literal is static (§1.4).

### 1.2 The `{arena}` Block

`{arena ... }` is a statement whose body is a statement list. It is parsed and type-checked, and the checker reports E5001 for a value that escapes it under the rule of §2.4. **It does not release memory.** Its body runs as any other block does, and the values made in it live under the same rules as values made anywhere else (§1.3).

What `{arena}` will release, and which stores out of it are rejected or copied, is decided by the ADR of story 127.525. A program that uses `{arena}` today shall not depend on memory being released at the block's exit. A program also should not let a value made in the block escape it (§2.4), because such a program may be rejected or behave differently once the ADR is implemented.

### 1.3 Release

Memory that the runtime allocates is not released before the program exits, with these exceptions:

1. **Closure environments.** A closure's environment struct is released when the last reference to it is released (§2.5, story 124.0c).
2. **The number conversions of a string interpolation.** In `"k\(i)=\(x)"` the conversion of each number segment is freed right after the literal's parts are joined into the result. No toke value can reach these conversions (story 127.299, `docs/decisions/127.299-function-arena.md` §6).
3. **Reused buffers.** The per-thread error buffer (§1.1) is reused, not released.

An implementation may release, at any time, memory that no toke value can still reach. It shall not release memory that a toke value can still reach. When the program exits, the operating system reclaims everything.

### 1.4 String Literals

String literals are statically allocated in the binary's read-only data segment and have program lifetime. Concatenation and the other string operations that produce new strings allocate as described in §1.1.

### 1.5 No Individual Deallocation

toke provides no mechanism for a toke program to free a single allocation. The releases in §1.3 are made by the runtime and by generated code, never by a toke expression. This is a deliberate design constraint, not an omission.

### 1.6 Leak Budget per Workload

Because §1.3 releases almost nothing, a program's resident memory grows with the total bytes it allocates. The budget below states what each kind of workload retains. It is a description of what ships, not a performance guarantee, and the figures it points to are measurements, not limits.

| Workload | What it retains | Budget |
|---|---|---|
| A program that runs to completion (a command-line tool, a batch job, a test, a corpus program) | Every value it allocated, until exit | The program's total allocation. The operating system reclaims it at exit. |
| A long-running loop or server (`http.serve`, `ooke serve`, a polling loop) | Every value each iteration or request allocated | Grows without bound by the bytes allocated per iteration or request. The implementation does not bound it. A program that must run indefinitely has to keep its per-iteration allocation small, or be restarted. |
| A string interpolation's number conversions | Nothing (§1.3 item 2) | Zero |
| A closure environment | Nothing once the last reference is released (§1.3 item 1) | Zero |
| A literal, a module-level constant | One copy, for the program's lifetime | Static (§1.4, §2.6) |

The unit of the budget is **bytes allocated per iteration** (sealed-heap bytes plus `malloc` bytes minus freed bytes), not peak resident memory. Peak resident memory varies between identical runs on some platforms. The measurement method and the reference figures for a template workload (ooke's lexer and renderer) are in `docs/decisions/127.299-function-arena.md` §1, §6 and §7. A program reports its own counters when it runs with `TK_MEM_STATS=1`: one line on stderr at exit, described with the harness recipe in `docs/runtime-abi.md` §10.1.

---

## 2. Lifetime Rules [N]

### 2.1 Value Lifetime

A value that the runtime allocates stays valid for as long as any toke name, field, element or capture refers to it. Under §1.3 it is in practice valid until the program exits. A value is never released while a toke value can still reach it.

### 2.2 Passing Values Down (Arguments)

Passing a value as a function argument is always safe. The callee can read the value for the duration of the call, and may store it, return it or capture it: the value stays valid (§2.1).

### 2.3 Passing Values Up (Return)

A function returns a value by handing its representation to the caller: a scalar by value, and a string, array, map or box by reference to the same allocation. Nothing is copied into a caller's region, because there is no per-function region to copy out of. The returned value stays valid in the caller for as long as the caller can reach it (§2.1).

Array copy-on-write (ADR-0006) preserves value semantics: a mutation of an array that another name still holds copies it first, so the caller cannot see a change it did not make.

### 2.4 Escape From an `{arena}` Block

E5001 is reserved for a value that escapes an `{arena}` block. As shipped, the checker's rule is narrow: inside an `{arena}` block, at any depth (in a nested `if` or `lp` body as well), assigning a value of a heap type (`str`, an array, a map, a struct or sum, a closure, or an error union whose value is one of these) to a module-level `mut` binding is E5001. The checker applies the rule when it knows the value's type; a value whose type it leaves unresolved is not rejected. A scalar (an integer, a float or a `bool`) is never allocated, so assigning one is not an escape. No other store, return or capture is rejected: an assignment to a binding of the enclosing function is accepted, and nothing outside an `{arena}` block is E5001. A module constant or a module-level immutable `let` cannot be assigned at all (E4070). Because `{arena}` releases nothing (§1.2), no escape leaves a dangling reference today.

Until story 127.526 (2026-11-24) the checker inspected the block's statement list as one node, so it never saw an assignment and E5001 was never emitted. The rule this section stated before 127.526, any assignment to a module-scope name, also rejected scalars, which never escape; the shipped rule is the part of it that matches the escape route the `{arena}` ADR proposes to reject (ADR-0016 D4, route E1).

The full escape rule, and whether an escape is rejected or copied out, is decided by the ADR of story 127.525 together with the release semantics of `{arena}`. A value bound in a nested block and returned from it is not an escape and is not E5001.

### 2.5 Closure Capture

Closures capture values by copy. When a closure is created, the values it references are copied into the closure's environment struct. The environment struct is allocated with `malloc` so that the closure can outlive the scope in which it was created.

The closure's environment struct is freed when the last reference to the closure is released. Generated code counts the references that bindings, copies, arguments, returns and captures hold, and releases a binding's reference at the binding's exits (story 124.0c). This counting is managed by the runtime and generated code and is not exposed to user code.

### 2.6 Static Lifetime

Module-level constants and declarations have static lifetime. They are initialised once at program start in declaration order and are never freed during execution. Circular initialisation dependencies are compile error E2020.

---

## 3. Concurrency and Memory [N]

### 3.1 Task Allocation

A spawned task (via `sc` structured concurrency) allocates from the same process-wide sources as every other task (§1.1). There is no per-task arena. The sealed heap's bump is atomic, and `malloc` is thread-safe.

### 3.2 No Shared Mutable State

Values shared between tasks must be copied at the point of task creation. There is no mechanism to share mutable memory between tasks. This eliminates data races by construction.

When a task is spawned, any values the task body references are copied for the new task. The spawning task and the spawned task then operate on independent copies.

### 3.3 Structured Concurrency Lifetime Guarantee

A parent task does not exit its `sc` block until all of its child tasks have completed. Child task lifetimes are therefore always contained within the parent's `sc` block.

### 3.4 Task Return Values

When a child task produces a result, the result is handed to the parent task as a value, following the same rules as function return values (§2.3).

---

## 4. FFI Boundary [N]

### 4.1 Pointers Passed to C

When toke calls a C function via FFI, toke values are passed as raw pointers to the underlying memory. The pointer is valid for the duration of the call, and under §1.3 it stays valid after the call too. A C function that keeps a pointer beyond the call depends on the current release rules, so it should document that, and the ADR of story 127.525 will decide how such retention is marked.

### 4.2 C Must Not Free toke Memory

C code must not call `free()` on pointers that originate from toke values: a sealed string, or a block the runtime allocated and handed to toke code. That memory is managed exclusively by the toke runtime and generated code. Calling `free()` on it is undefined behaviour.

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
| Garbage collector | Not present | Memory is not reclaimed during execution (§1.3), so there is nothing to collect and no pause times. The cost is the leak budget in §1.6. |
| Arenas | Not implemented | The per-function arena of the pre-127.524 text was never implemented. `{arena}` parses and type-checks but releases nothing (§1.2). Release by `{arena}` is the subject of story 127.525. |
| Reference counting | Internal only, not exposed | Closure environments are counted and released (§2.5). Arrays carry a monotonic count that only decides copy-on-write and never frees (ADR-0006). No toke code sees or manipulates a count. |
| Borrow checker | Not present | Values are copied or shared under copy-on-write (§2.3), and nothing a toke value can reach is released (§1.3), so there is no borrow to check. |
| Manual malloc/free | Not present in toke code | Only accessible through C FFI. toke code cannot free an allocation (§1.5). |
| Weak references | Not present | No reference system is exposed to have a weak variant of. |
| Finalizers/destructors | Not present | No release runs per-object cleanup. Resources requiring cleanup (file handles, connections) use explicit close calls. |

---

## 6. Formal Properties [N]

A conforming toke implementation shall guarantee the following properties for well-typed toke code that does not use FFI:

### 6.1 No Use-After-Free

No memory that a toke value can reach is released (§1.3). The only releases before exit are of closure environments whose last reference has been released, and of interpolation conversions that no toke value can reach. Therefore no reference can be used after the memory it refers to is freed.

### 6.2 No Double-Free

There is no operation in toke code to free an allocation (§1.5). Each release in §1.3 frees an allocation exactly once: an interpolation conversion right after its join, and a closure environment when its count reaches zero. Therefore no allocation can be freed more than once.

### 6.3 No Dangling Pointers

Every pointer a toke value holds refers to memory that is still allocated (§2.1, §6.1).

### 6.4 Memory Growth

Memory is not bounded. Retained memory grows with the total bytes the program allocates, less the releases of §1.3. §1.6 states what each workload retains. `{arena}` does not bound per-iteration allocation until the ADR of story 127.525 gives it release semantics.

### 6.5 No Data Races

Tasks do not share mutable memory (Section 3.2). All inter-task communication is by copy. Therefore concurrent access to the same mutable memory location cannot occur.

### 6.6 Spatial and Arithmetic Safety [N]

A conforming implementation **traps deterministically** — aborting with a diagnostic rather than executing undefined behaviour — on the following out-of-contract operations:

| Code  | Condition                             | Guard |
|-------|---------------------------------------|-------|
| RT002 | Signed integer overflow (`+ - *`)     | Checked arithmetic (`llvm.*.with.overflow`), `tk_overflow_trap`. |
| RT003 | Array subscript outside `[0, len)` — `.get`, `.set`, and a `std.vec` handle's `get`/`set` | Length read from the array header (`base[-1]`), or from `tk_vec_len_w` for a `std.vec` handle, `tk_bounds_trap`. Default-on; the optimizer elides provably-in-range checks at `-O2`. |
| RT004 | Division or remainder by zero, at every integer width; signed division or remainder of the most negative value by `-1` | Divisor tested before `sdiv`/`srem`/`udiv`/`urem`, and the (MIN, -1) pair before `sdiv`/`srem`, `tk_div_trap`. |
| RT005 | Nil dereference of a struct field     | Struct base tested for null before the field GEP, `tk_nil_trap`. |
| RT010 | `!` re-raises a user error into a different error type, on a path the checker (E4076) could not judge | Both types' layouts compared at the re-raise; `tk_reraise_trap` names the two types (127.225). |

**Narrow integers wrap (ADR-0012 c).** RT002 covers `i64` and `u64`. The sized types `i8`, `i16`, `i32`, `u8`, `u16` and `u32` do **not** trap on `+ - *` overflow: the result wraps in two's complement at the type's width, identically at `-O0` and `-O2` (`127 as i8 + 1` is `-128`; `255 as u8 + 1` is `0`). This is defined behaviour, not undefined behaviour. Division is the exception: RT004 above applies at every width.

Every emitted function additionally carries a stack canary (`sspstrong`), so a stack-buffer overflow aborts rather than redirecting control flow.

**Rationale.** The compiler is the root of trust for the training corpus. Deterministic traps make a program's observable behaviour a total function of its inputs — never a function of uninitialised or out-of-bounds memory — a precondition for a differential-tested, reproducible corpus.

### 6.7 Injection Resistance [N]

The standard library is injection-resistant by construction on its guaranteed surfaces:

- **Process execution is argv-only.** `process.spawn`/`exec`/`readlines`/`detach` never invoke a shell; a command built from untrusted data cannot inject a second command (shell metacharacters are inert arguments).
- **HTML is context-aware auto-escaped by default.** Interpolated template values (`tpl.render`) and HTML attribute values are HTML-escaped; URL-context attributes (`href`/`src`/…) drop dangerous schemes (`javascript:`/`vbscript:`/non-image `data:`); `<script>`/`<style>`/`<title>` content is breakout-guarded; markdown rendering neutralises raw HTML from the source. Raw HTML output requires the explicit `|raw` opt-out.

---

## 7. Implementation Notes [I]

### 7.1 The Sealed Heap and `malloc`

The layout of a sealed string, the census of converted producers and the rule that a sealed string is never freed are specified in `docs/runtime-abi.md` §3. The array header (`rc | cap | len`) and copy-on-write are specified in ADR-0006.

### 7.2 Releasing Memory in Future

An implementation that releases memory a toke value can still reach does not conform (§1.3). A future release mechanism, beginning with `{arena}`, must state its escape rule, its interaction with the sealed heap's never-freed rule, and how C code that keeps a toke pointer is marked. Story 127.525 holds that ADR. The options and their proof obligations are in `docs/decisions/127.299-function-arena.md` §2 to §5.

---

## References

- toke Language Specification v0.3, Section 14 — Memory Model
- toke Language Specification v0.3, Section 15.5 — Interface Files
- toke Language Specification v0.3, Section 24.2 — FFI (deferred)
- `docs/runtime-abi.md` §3 — the sealed-string heap (S1)
- `docs/decisions/ADR-0006.md` — array copy-on-write and the monotonic count
- `docs/decisions/127.299-function-arena.md` — design input for this amendment (127.524) and for the `{arena}` ADR (127.525)
