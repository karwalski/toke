# Known Limitations — toke compiler v0.4

Last updated: 2026-07-03 (empirically re-verified against the reference compiler)

This document lists known limitations, workarounds, and planned fixes for the
toke compiler v0.4 release. It covers the language, code generation, build
system, and runtime. Each entry marked "verified 2026-07-03" was confirmed by
compiling and *running* a minimal program (exit-code check), not just `--check`
— several prior entries were stale in both directions (bitwise operators are now
implemented; anonymous functions now compile but miscodegen).

---

## Language Limitations

### 1. Unit enum variants require payload types

Unit enum syntax like `t=$status{$active;$inactive}` is not supported. Variants
must have explicit payload types.

**Workaround:** Write `t=$status{$active:i64;$inactive:i64}` and use a
sentinel value (e.g. 0) as the payload.

**Planned fix:** Story 85.1.11 (backlog). Migration tooling may automate this
conversion in a future release.

### 2. Mutable reassignment requires `mut` binding

`let x=5; x=10` fails at compile time. The compiler does not allow
reassignment to an immutable binding.

**Workaround:** Use `let x=mut.5; x=10`.

**Planned fix:** None planned. This is intentional — mutability must be
declared explicitly.

### 3. Anonymous functions / closures miscodegen — DO NOT USE

`fn(params){body}` anonymous functions parse and typecheck, but the emitted code
is **incorrect at runtime**. Verified 2026-07-03: a pure `fn(x:i64):i64{<x+1}`
called with `7` returns **`7`, not `8`** (the body computation is lost), and
captured enclosing variables read as **`0`** (`fn(x){<x+base}` with `base=10`
returns `x` only). The v0.4 capture-by-value design (76.1.9: `{fn_ptr,env_ptr}`
pair) is not correctly wired, and even the non-capturing case is wrong.

**Workaround:** Use named top-level functions; pass all needed values as explicit
parameters and reference with `&name`. This is a **silent wrong-value** bug — it
does not error, so avoid `fn(...)` entirely until fixed.

**Status:** Codegen correctness bug — tracked in **Epic 123.5**.

### 4. No generics or traits

There is no parametric polymorphism or trait/interface system. Functions and
types cannot be generic over type parameters.

**Workaround:** Write concrete implementations for each type, or use `i64`
(the universal toke scalar) where possible.

**Planned fix:** No timeline set. Generics are deferred indefinitely per the
spec (Section 24).

### 5. Option type is partial; the `$none` match arm miscodegen's

`$none` exists as a built-in zero-field struct (`stdlib/option.tki`; `$none{}` is
a value; the `T!$none` convention reuses the error-union machinery). Verified
2026-07-03: a function returning `T!$none` compiles and the `$ok` path is correct
(`find(5)→5`), **but the `$none` match arm returns `0` instead of the arm's
value** (`mt find(0){$ok:v v;$none:e 42}` returns `0`, not `42`) — a codegen bug
in the same family as #3. There is no `$some` wrapper or distinct `$option<T>`.

**Workaround:** For optional returns, prefer a sentinel or
`$result{$ok:T;$err:$str}` until the `$none`-arm codegen is fixed.

**Status:** Codegen correctness bug — tracked in **Epic 123.5**.

### 6. No concurrency primitives beyond `std.task`

`std.task` provides `task.scope()`, `task.spawn(scope; &fn)`,
`task.await_all(scope)`, and `task.result(handle)` — basic structured
concurrency with a thread pool. There are no channels, mutexes, atomics, or
`sc`/`spawn` keywords.

**Workaround:** Use `std.task` for coarse-grained parallelism. For HTTP
servers, ooke uses pre-fork for scaling.

**Planned fix:** `sc`/`spawn` keywords deferred to v0.5 (Story 76.1.1b).
Formal memory model documented (Story 76.1.4).

### 7. ~~Bitwise operators deferred~~ — IMPLEMENTED

`&` `|` `^` `<<` `>>` are implemented and verified at runtime (2026-07-03:
`5&3=1`, `5|2=7`, `5^3=6`, `1<<3=8`, `8>>1=4`). Precedence chain is
`BitOr → BitXor → BitAnd` (`src/parser.c` `parse_bitor`/`parse_bitxor`/
`parse_bitand`). Note single `|` is bitwise-OR (distinct from `||`). A dedicated
`~` (bitwise NOT) prefix operator is not confirmed here — use `(0-1) ^ x` if a
NOT is needed, or verify `~` separately.

### 8. `@wrapping` overflow annotation not implemented

Integer arithmetic uses checked overflow by default (traps on overflow via
RT002). The `@wrapping` opt-out annotation is specified but not yet
implemented.

**Workaround:** None. All integer arithmetic is checked.

**Planned fix:** Deferred to a future story (noted in 10.3.1b).

---

## Code Generation Limitations

### 1. `as $str` on integers emits `inttoptr`

Casting an integer to `$str` with `as $str` emits an LLVM `inttoptr`
instruction instead of calling the `str_from_int` runtime function. The result
is a garbage pointer, not a string representation of the number.

**Workaround:** Use `str.fromint(n)` for integer-to-string conversion.

**Planned fix:** Tracked. The `str_from_int` C function exists in the stdlib
(one of 4 functions that must remain in C per Story 74.1.1).

### 2. String interpolation `\(expr)` unreliable for some types

String interpolation works in the default profile but does not evaluate at
runtime for all types. Non-string, non-integer expressions inside `\(...)` may
produce unexpected results.

**Workaround:** Use `str.concat()` for reliable string building across all
types.

### 3. Match expressions are expression-only

Match (`mt`) is an expression that yields a value; it is not a side-effect
statement. **As of 116.1/A1, `if`/`el` is *also* an expression** (`let x=if(c){a}el{b}`,
`<if(c){a}el{b}`, with `el if` chaining) — the expression form requires an `el`
branch (a value on every path). `if` also still works as a plain statement.
Prefer the expression form over the old `let x=mut.0; if(c){x=a}el{x=b}` pattern.

### 4. ~~Return operator `<` cannot be used inside match arms~~ — RESOLVED

Fixed by 114.47: `<expr` in a match arm is an early return
(`emit_match_arm_body` emits the function return). Verified 2026-07-01:
`let r=mt st.toint(x){$ok:v v; $err:e <0}` returns `0` on the error arm.

### 5. Array append syntax creates a new array

`@(existing_arr; new_item)` creates a 2-element array literal, not an append
operation. It does not extend the existing array.

**Workaround:** Use `.push()` for appending to an existing array:
`existing_arr.push(new_item)`.

### 6. ~~`=` is equality, not assignment~~ — CHANGED (A3, 116.3)

As of v0.4 (Epic 116 / A3), **`=` is assignment/binding and `==` is equality**
(the conventional split, and strict-LL(1) — it removed the `=`-overload's
unbounded loop-init lookahead). `let x=5`, `x=10`, `lp(let i=0;i<n;i=i+1)` use
`=`; comparisons use `==` (`if(x==5)`). A bare `=` in expression position is a
compile error (E2002, "use `==`"). Migrate old sources with
`scripts/migrate_eq.py`.

### 7. Struct field map access generates incorrect code

`is_map_var()` in the compiler does not recognize maps extracted from struct
fields. Accessing a map that was obtained from a struct field generates array
indexing instead of `tk_map_get()`, causing SIGBUS.

**Workaround:** Extract the map into a dedicated typed field on the struct
rather than accessing it dynamically.

**Reference:** Story 56.10.4.

### 8. Cross-module function references not supported

`&mod.func` syntax for referencing a function from another module does not
work. Only local function references (`&name`) are supported.

**Workaround:** Create a local wrapper function that calls the imported
function, then reference the wrapper: `f=wrapper(){mod.func()}` and use
`&wrapper`.

**Reference:** Story 82.1.6.

---

## Build System Limitations

### 1. Separate compilation and manual linking

Each `.tk` file must be compiled separately, then linked with clang. There is
no single-command whole-program compilation.

**Workaround:** Use `toke --emit-deps` to get the list of required stdlib C
files, then link with clang:

```
toke --emit-llvm main.tk
clang main.ll $(toke --emit-deps main.tk) -o main
```

**Planned fix:** Multi-file batch compile (`toke --emit-llvm main.tk mod.tk`)
is backlogged (Story 81b.5).

### 2. Interface files must be in CWD or search path

`.tki` files (module interfaces) must be in the current working directory or a
directory specified with the `-I` flag. The compiler does not search a standard
library path automatically.

**Workaround:** Use `-I /path/to/toke/stdlib` or symlink the stdlib directory
into your project.

### 3. Stdlib C files must be linked explicitly

The toke stdlib is implemented in C. You must link the required C source files
(e.g. `str_glue.c`, `collections_glue.c`, `collections.c`) when building your
program.

**Workaround:** Use `toke --emit-deps` to get the full list of required files.
Note that `str_glue.c`, `collections_glue.c`, and `collections.c` are always
required (array/map built-in methods like `.push`, `.get`, `.append` depend on
them).

### 4. No incremental compilation

Any change to any source file requires a full recompile. There is no dependency
tracking or caching of intermediate results.

**Planned fix:** No timeline set.

---

## Runtime Limitations

### 1. No garbage collection — arena allocator, all allocations freed on exit

toke uses an arena-based memory model. Allocations are never individually freed
during program execution. All memory is released when the process exits.

For long-running servers (e.g. ooke), each spawned task gets its own arena
(Story 76.1.1a), but within a task, allocations accumulate.

**Planned fix:** Formal memory model is documented (Story 76.1.4). Arena-aware
return value copying is verified correct (Story 76.1.4b). No GC is planned.

### 2. No stack overflow detection

Deep recursion will segfault without a meaningful error message. The runtime
spec defines a recursion limit of 1000 frames (RT005) but this is not enforced
at the compiler or runtime level.

**Workaround:** Avoid deep recursion. Convert recursive algorithms to iterative
versions using loops (`lp`).

### 3. No array bounds checking

Out-of-bounds array access produces a segfault. There is no runtime bounds
check or trap.

**Workaround:** Check array length before access. Use `.get()` methods where
available.

**Planned fix:** Runtime bounds traps are specified (RT003) but not yet
implemented in codegen.

---

## Diagnostic Limitations

### 1. Single-error recovery

The compiler stops at the first error in name resolution and type checking.
It does not collect and report multiple errors in a single pass.

**Planned fix:** Multi-error recovery is backlogged for both name resolution
(Story 84.1.8) and type checking (Story 84.1.9).

### 2. No source line display in error output

Diagnostic messages show file, line, and column but do not display the actual
source line or a caret pointing to the error location.

**Planned fix:** Stories 84.1.14 (source_line field) and 84.1.15
(human-readable display with caret) are backlogged.

### 3. Limited "did you mean?" suggestions

Unresolved name errors (E3011) do not suggest similar names via fuzzy matching.

**Planned fix:** Story 84.1.3 (Levenshtein-based suggestions) is backlogged.

---

## Package Management

There is no package manager. The `tkc pkg` CLI commands (init, add, remove,
resolve, fetch, list) are backlogged (Stories 76.1.3a-d). Package registry is
deferred to v0.6+.
