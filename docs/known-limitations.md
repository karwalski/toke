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

### 3. Closures / anonymous functions — IMPLEMENTED (capture by value)

`fn(params){body}` anonymous functions now **compile and run correctly** (Epic
124.0a, 2026-07-04). The prior silent-wrong-value miscodegen is fixed: non-capturing
and capture-by-value closures, direct application `fn(x){<x+1}(7)`, binding + call
`let f=fn(..){..}; f(a)`, closures passed/returned, and **nested closures** all
produce correct values (verified `test/standalone/test_closures.tk`, 6/6). A closure
value is a heap box `{fn_ptr, captured…}` (the `{fn_ptr,env_ptr}` design of 76.1.9);
the lifted function is emitted at module scope and invoked via an indirect call.

**Remaining follow-ups (do not block use for the common i64/pointer case):**
- The env is `malloc`'d and **not freed until process exit** — ref-counted free
  (memory-model §2.5) is a follow-up. No use-after-free; just retained memory,
  consistent with the arena model.
- Closures are **not yet type-checked** (`types.c` returns `TY_UNKNOWN` for a
  closure-valued call — call arity / arg / return types are unchecked). A soundness
  follow-up (its own ADR-adjacent story); codegen is correct.
- `f64` / array / map **closure params** aren't covered by the i64/pointer ABI yet.
- The lexer emits a **spurious `W1020` "Rust keyword 'fn'"** warning on the valid
  `fn(` closure form (harmless — it's a warning; a lexer fix is pending).

**Status:** Codegen correctness bug — tracked in **Epic 123.5**.

### 4. No generics or traits

There is no parametric polymorphism or trait/interface system. Functions and
types cannot be generic over type parameters.

**Workaround:** Write concrete implementations for each type, or use `i64`
(the universal toke scalar) where possible.

**Planned fix:** No timeline set. Generics are deferred indefinitely per the
spec (Section 24).

### 5. Option type via `T!$none` — WORKS (no `$some` wrapper)

`$none` exists as a built-in zero-field struct (`stdlib/option.tki`; `$none{}` is
a value; the `T!$none` convention reuses the error-union machinery). The prior
`$none`-arm miscodegen is **fixed** (Epic 124.0a, 2026-07-04): `mt find(0){$ok:v
v;$none:e 42}` now returns `42`, the `$ok` path returns the value, and an ok value
of `0` is correctly distinguished from `$none`. Root cause was the return path —
`<$none{}` stored `$none`'s zero box into `@tk_current_error` (colliding with the
"no error" sentinel); the fix stores a non-zero flag for `$none` returns.

**Remaining:** there is still no `$some` wrapper or distinct `$option<T>` type, and
closures/error-unions are not yet type-checked (see #3 follow-ups / ADR-0014).

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

### 1. ~~`as $str` on integers emits `inttoptr`~~ — RESOLVED

`n as $str` now calls `str_from_int` (via `tk_str_fromi64_w`) and returns the
decimal string (Story 101.R3b; verified 2026-07-07: `42 as $str` → `"42"`).

### 2. String interpolation `\(expr)` — numbers/strings work; composites error

`\(x)` correctly stringifies `str`, `i64`, narrow ints, `bool`, and `f64`
(via `tk_str_fromi64_w`/`tk_str_fromfloat_w`). Interpolating an **array** (and a
struct whose type the compiler can resolve) is now a **compile error**
(`E4032` — "cannot interpolate a composite value into a string") rather than the
previous silent garbage (123.5, 2026-07-07). **Residual:** a struct/map whose
static type resolves to unknown (`est==NULL`, e.g. some `.get()` results) still
falls through to the string path and can misbehave — this can't be tightened
without regressing real strings from `str.concat` (which also have `est==NULL`);
it needs interpolation-context type tracking (follow-up). Since 123.11-fu, E4032
also fires under `--check` for composites the type checker can definitively type
(direct literals like `\(@(1;2;3))`, struct field accesses like `\(o.inner)`,
and annotated composites); bare composite *locals* (which the checker
conservatively types as unknown to limit E4031 blast radius) remain caught at
codegen.

**Workaround:** interpolate the elements/fields, or use `str.concat()`.

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

### 7. ~~Struct field map access generates incorrect code~~ — RESOLVED

`.get()` on a map stored in a struct field now correctly routes to
`tk_map_get()` — verified 2026-07-07 for direct (`b.meta.get("k")`), extracted-
to-local, and nested (`o.inner.meta.get("k")`) access, all returning the right
value. The old `[]`-index path that caused the SIGBUS is unreachable: square-
bracket indexing was removed in v0.4 (`E1003`; use `.get()`). Reference: Story
56.10.4.

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

Deep recursion will segfault without a meaningful error message. There is no
enforced recursion-frame limit at the compiler or runtime level.

**Workaround:** Avoid deep recursion. Convert recursive algorithms to iterative
versions using loops (`lp`).

### 3. ~~No array bounds checking~~ — RESOLVED (RT003)

Array/collection index access is now bounds-checked in codegen: an out-of-range
index traps `RT003: index N out of bounds for length M` and exits, rather than
segfaulting. Divide-by-zero traps `RT004`, and dereferencing a nil struct base
traps `RT005: nil dereference` (the map-`.get`-miss-returns-0 case). Under `-O2`
provably-in-range checks are eliminated. See [runtime-abi.md §9](runtime-abi.md)
and ADR-0012.

### 4. Capabilities are deny-by-default (ADR-0010)

A compiled program has **no** fs/net/env-write/process-spawn authority unless it
is granted — via `tkc.toml [capabilities]` / `--allow-*` at compile time (baked)
or `--allow-*` at run time. An ungranted sink fails closed with `CAP001`.
Consumed `--allow-*` flags are stripped from the program's own argv. One
coarsening remains: scoped grants (`--allow-net=host`, `--allow-read=/path`) are
honoured at the **class** level (per-path/per-host scoping is a planned
refinement). See [spec/capabilities.md](spec/capabilities.md).

### 5. Ambient-hardening behavioural changes (124.4h)

Defense-in-depth hardening of the stdlib changes some filesystem/exec behaviour:
- **`O_NOFOLLOW` on file opens** (`file.read`/`write`/`append`/`copy`, `os.open`):
  opening a path whose final component is a **symlink** now fails (`ELOOP`) rather
  than following it. Programs that intentionally read/write through a symlink must
  target the link's real path.
- **PATH is snapshotted at startup** for `process.exec`/`spawn`: a bare command is
  resolved against the PATH captured before any program code runs, so a later
  `env.set("PATH", …)` / dotenv load cannot repoint command lookup. Absolute/`/`-
  containing commands exec directly.
- **`os.read`/`os.write` are string-based** (`os.read(fd, count) -> str`,
  `os.write(fd, data:str)`) — the old raw integer-as-buffer-pointer form is gone.
  Embedded-NUL binary data is a known limitation of the string form (a `[byte]`
  variant is a follow-up).
- **dotenv (`env.file_load`) refuses** `LD_*`/`DYLD_*`/`PATH`/`IFS` keys; explicit
  `env.set` is unaffected.
- Recursive delete (`file.rmdir_r`) uses `lstat`, so a symlink inside the tree is
  removed as a link and never followed to delete external files.

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
