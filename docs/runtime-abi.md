# Toke Runtime ABI v0

Binary interface between compiled toke programs and the C runtime
(`tk_runtime.h` / `tk_runtime.c`).

Story: 10.4.9

---

## 1. Calling Convention

All functions use the **C calling convention** (`ccc` in LLVM IR).

The toke `main` function is renamed to `tk_main` during codegen.
The compiler emits a C-compatible `main()` wrapper:

```llvm
define i32 @main(i32 %argc, ptr %argv) {
  call void @tk_runtime_init(i32 %argc, ptr %argv)
  %r = call i64 @tk_main()
  %rc = trunc i64 %r to i32
  ret i32 %rc
}
```

If `tk_main` returns `void`, the wrapper calls it without capturing the
return value and returns `i32 0`.

**Exit code**: the i64 return value of `tk_main` is truncated to i32 for
the process exit code.

---

## 2. Scalar Types

| Toke type | LLVM IR type | Size    | Representation                |
|-----------|-------------|---------|-------------------------------|
| `i64`     | `i64`       | 8 bytes | Signed two's complement       |
| `f64`     | `double`    | 8 bytes | IEEE 754 double precision     |
| `bool`    | `i1`        | 1 bit   | 0 = false, 1 = true           |
| `void`    | `void`      | 0 bytes | No value                      |

**Bool widening**: when `i1` values cross a boundary that expects `i64`
(arithmetic, function call arguments, return statements), the compiler
emits `zext i1 %val to i64`. This is a zero-extension (false = 0,
true = 1).

---

## 3. String Layout

Strings are **C strings**: null-terminated `ptr` (pointer to `i8`).

- String literals are emitted as LLVM global constants
  (`@.str.N = private constant [K x i8] c"...\\00"`).
- Runtime-created strings (e.g. from `tk_str_concat`) are allocated
  via `malloc`. **Caller owns returned strings** -- the runtime does
  not track or free them.

**IR access pattern** for string literals:
```llvm
%t0 = getelementptr inbounds [K x i8], ptr @.str.0, i32 0, i32 0
```

---

## 4. Array Layout

Arrays use a **length-prefixed** layout where all elements are `i64`:

```
Memory: [ len | data[0] | data[1] | ... | data[len-1] ]
         ^      ^
         block   ptr (what toke code sees)
```

- The pointer exposed to toke code points to `data[0]`.
- The length is stored at `ptr[-1]` (i.e. one i64 before the data pointer).
- Element type is `i64` (8 bytes). Non-integer values such as pointers
  to strings or nested arrays are stored as `i64` via `ptrtoint`/`inttoptr`.

**Stack-allocated array literals**:
```llvm
%t0 = alloca i64, i64 4          ; block: len + 3 elements
%t1 = getelementptr inbounds i64, ptr %t0, i64 0
store i64 3, ptr %t1             ; store length
%t2 = getelementptr inbounds i64, ptr %t0, i64 1  ; data start
; store elements at %t2 + offset...
```

**Heap-allocated arrays** (runtime operations like concat): allocated
via `malloc((len + 1) * sizeof(i64))` in the C runtime. The returned
pointer points to `data[0]`.

**Length access** (`.len` property):
```llvm
%t0 = getelementptr inbounds i64, ptr %arr, i32 -1
%t1 = load i64, ptr %t0
```

**Element access** (indexing):
```llvm
%t0 = getelementptr i64, ptr %arr, i64 %idx
%t1 = load i64, ptr %t0
```

---

## 5. Struct Layout

Structs are emitted as flat `i64` arrays (not LLVM named struct types).
All fields are `i64`-typed, with non-integer values stored via pointer
casts.

**Allocation**: heap-allocated via `malloc`, eight bytes per field. The
local that names the struct is an `alloca` holding the *pointer*, not the
fields:
```llvm
%t0 = call i8* @malloc(i64 24)  ; struct with 3 fields
%p  = alloca i8*                ; the binding, pointing at %t0
```

Struct values are therefore safe to return and to outlive the frame that
built them — which is what lets an error record survive the return that
raised it (§7.3). (Corrected 127.97: this section previously described an
`alloca i64, i32 N` layout that the compiler has never emitted.)

**Field access**: via `getelementptr` with a compile-time field index
determined by `struct_field_index()`:
```llvm
; Write field at index 1:
%t1 = getelementptr inbounds i64, ptr %t0, i32 1
store i64 %val, ptr %t1

; Read field at index 1:
%t2 = getelementptr inbounds i64, ptr %t0, i32 1
%t3 = load i64, ptr %t2
```

The compiler maintains a `StructInfo` registry that maps field names to
indices. Field order matches declaration order in the source.

---

## 6. Map Layout

**[TODO]** -- Maps (`[K:V]`) are defined in the type system (`TY_MAP`)
but do not yet have a codegen representation in `llvm.c`.

---

## 7. Error Union Layout

Error unions (`T!Err`) are **split across two channels**: the ordinary
return register carries the success value, and a separate runtime slot
carries the error. There is no two-word return and no tagged value.

Stories: 114.41, 114.55, 124.0a, 127.56, 127.97.

### 7.1 The two channels

| Channel | Symbol | Type | Carries |
|---------|--------|------|---------|
| Value | the function's own return register | `T` lowered per §2–§6 | the success value, or a zero/null filler on the error path |
| Error | `@tk_current_error` | `i64` | the discriminant **and** the payload |

A `T!Err` function lowers to a plain `T`-returning function. Nothing in
its LLVM signature records that it is fallible; the error type is known
statically at every call site from the declaration, and that is what the
consumer uses to decode the slot.

### 7.2 `@tk_current_error` is tri-valued

This single `i64` is both the discriminant and the payload pointer:

| Value | Meaning |
|-------|---------|
| `0` | **Success.** The value channel holds a real `T`. |
| `1` | **Failure with no payload.** The reason is not recoverable. |
| anything else | **Failure with a payload.** The value is a pointer to an error record (§7.3), cast through `ptrtoint`. |

`1` is reserved and is never a valid payload pointer, because every
payload is `malloc`'d. It is what the C glue stores (`analytics_glue.c`,
`image_glue.c`, …), what a `<$none{}` return stores (124.0a — `$none`
declares no fields, and storing its natural box value of `0` would read
back as success), and what `!` propagation stores when the callee it
propagated from used the older value-sentinel convention (127.56).

**The zero filler is not the discriminant.** An ok return of `0`, `0.0`,
`false`, `""` or an empty array is indistinguishable from the error
filler in the value channel, so consumers must decide on the slot, never
on the value. This is why every ok return from a fallible function
explicitly stores `0` into the slot (114.55) — the clear happens *after*
the return expression is evaluated, so a call inside that expression
cannot leave a stale error behind.

**The slot belongs to one call, so a binding captures it (135.16).** The
slot is live global state: by the time a consumer looks, any later
fallible call has overwritten it. So when a fallible call is bound —
`let r = file.size(p)` — the compiler spills the slot into a local of
that binding's own, taken immediately after the call returns, and a
later `mt r` decides on **that** copy. Without the capture the two
spellings of one call disagree: `mt file.size(p)` read the slot and
`let r = file.size(p); mt r` fell back to the value sentinel, so an
empty file's size of `0` was reported as a failure — silently, for
`str.toint`/`tofloat`, `toml.i64`/`bool`, `file.size` and every user
`T!$err` whose ok value can be `0`. Re-assigning the binding re-points
it at the new call's slot; a shadowing re-bind gets its own.

### 7.3 Error record layout

The payload is a heap record. It must not be an `alloca` — it may
outlive the frame that raised it.

**Ownership (127.109).** It is *not* `malloc`'d per error. Each thread owns
exactly one error-box buffer (`tk_err_box`, `tk_runtime.c`), **reused by every
raise on that thread**. A box is valid until the next raise on the same
thread, which is exactly the window restriction 1 in §7.5 already defines for
the `$err` arm's binding — so the rule adds no restriction, and there is
nothing to free per error.

The buffer is an inline thread-local array sized for the common box (a 2-slot
sum, or a record of up to 8 fields), so the usual error return performs **no
allocation at all** and cannot fail. Only a wider error type reaches the heap;
that buffer is released at exit for the main thread, and a worker that raised
one leaves a single buffer at thread exit — constant, not
workload-proportional. `tk_err_box` never returns null, because a null box
would be stored into the slot as `0`, and `0` is success: an out-of-memory
error return would read back as a successful one.

Until 127.109 every error return and every `!` propagation called `malloc` and
the emitted code contained **no `free` at all**: 16 bytes per error ever
raised. `patterns/err-default` at N=64e6 allocated 9,142,858 boxes and
146 MB, with the free count constant in N. It is now 0 allocations for the
error channel — 32 for the whole process, the same as a program that raises
nothing — and 1.4 MB peak, constant in N. The idiom is at parity with the
precondition-guard form it used to lose to by five orders of magnitude
(127.49), on both allocations and wall time (0.12 s either way at N=64e6).

**Sum-typed errors** (`t=$err{$bad:$str;$worse:i64}`) are a 2-slot box:

```llvm
%box = call i8* @malloc(i64 16)
; slot 0: i64 tag     — the variant's declaration index
; slot 1: i64 payload — the variant's value, bitcast/ptrtoint to i64
```

The tag `-1` is reserved: it marks an error synthesised by `!`
propagation from a callee that carried no variant of its own, so a
nested `mt e {$variants}` reads a well-formed box and falls through to
its last arm rather than dereferencing a sentinel (127.56).

**Record-typed errors** (`t=$myerr{code:i64;msg:$str}`) are an ordinary
struct per §5 — a `malloc`'d flat `i64` array, one slot per field, in
declaration order.

### 7.4 Consuming an error union

`mt call() {$ok:v …;$err:e …}` lowers to a branch on the slot. The
`$ok` arm binds the value channel. The `$err` arm binds the slot, after
normalising the reserved `1` to nil — so an arm that reads a field of a
payload-less error takes a clean RT005 nil trap (§9) rather than
dereferencing the address `1`.

`expr!$Err` propagates: it branches on the same slot, and on the error
path re-raises into the caller's declared error type and returns the
zero filler.

### 7.5 Restrictions (as implemented)

These are limitations of the current lowering, not of the design:

1. **The payload is bound only when the `mt` scrutinee is the call
   itself.** `let r = f(x); mt r {…}` binds nil in the `$err` arm,
   because the slot is a single global and any call between the `let`
   and the `mt` overwrites it. Match on the call directly.
2. ~~**The slot is process-wide, not thread-local.**~~ **Fixed, 127.101.**
   It is thread-local, as the compiler comments always claimed and this
   document previously denied. `tk_runtime.c` defines it `TK_TLS` and
   `llvm.c` emits `@tk_current_error = external thread_local global i64`.
   The two spellings are load-bearing: a plain-global declaration against a
   TLS definition links with **no diagnostic** and then takes SIGBUS on the
   first access, so every translation unit that declares the slot — the
   twelve glue files included — must use the macro. `TK_TLS`, not C11
   `_Thread_local`, because `tk_runtime.c` is built with `-std=c99
   -Wpedantic -Werror`, under which `_Thread_local` is a hard error.

   This was reachable, not theoretical: `task.c`'s `pool_worker` calls a
   toke function pointer (`t->result = t->fn()`) on a pool thread, so two
   `std.task` tasks raising errors really did overwrite each other's.
3. **Nothing enforces that every error exit sets the slot** (127.59).
4. **The declared error type is not checked against what is
   observable.** `FileErr` declares three variants while
   `file.lasterrkind()` distinguishes ten; `MdnsErr` declares five and
   no function returns it.

### 7.6 Why this representation

The obvious alternative is a two-word return — `{i64 tag, i64 payload}`
by value, or an `sret` out-parameter — which needs no global, is
re-entrant, and composes. It was **not** chosen here, because it changes
the LLVM signature of every fallible function: 132 fallible functions
across 33 stdlib modules, every C glue wrapper behind them, all 63
error-union declarations in ooke, and the 1,253 corpus records that
call them.
<!-- facts-exempt: 2026-09-20 the counts in the paragraph above are the tree as
     it stood when this decision was taken (114.41-era: 33 stdlib modules, 1,253
     corpus records). They size a decision already made, so re-deriving them to
     today's numbers would rewrite the record rather than correct it. Story
     132.36. --> That is a coordinated break of the whole tree, and the
compiler is the root of trust for the corpus (AGENTS.md §1).

The split-channel form, by contrast, is **already the ABI** — the return
path has stashed a typed box since 114.41. What was missing was only
that the consumer read it back, and a written definition of the slot's
three states. Both are supplied above.

Restriction 2 was named here as the trigger that would force the question.
127.101 answered it the cheaper way — a thread-local slot — so the trigger
is spent and the two-word return is **not** required for concurrent error
reporting. What would still force it is a caller needing two error payloads
live at once, which restriction 1 forbids independently of this.

---

## 8. Runtime Functions

All functions are declared in `tk_runtime.h` and linked from
`tk_runtime.c`.

| Function | IR Signature | Purpose | Allocation |
|----------|-------------|---------|------------|
| `tk_runtime_init` | `void (i32, ptr)` | Store argc/argv globals for later access | None |
| `tk_str_argv` | `ptr (i64)` | Return `argv[index]` as a C string | None (returns pointer into argv) |
| `tk_json_parse` | `i64 (ptr)` | Parse JSON string into a toke value | malloc for arrays and strings |
| `tk_json_print` | `void (i64)` | Print i64 value as JSON to stdout | None |
| `tk_json_print_i64` | `void (i64)` | Print i64 as decimal integer | None |
| `tk_json_print_bool` | `void (i64)` | Print i64 as `true`/`false` | None |
| `tk_json_print_str` | `void (ptr)` | Print C string as JSON quoted string | None |
| `tk_json_print_arr` | `void (ptr)` | Print length-prefixed i64 array as JSON | None |
| `tk_json_print_arr_bool` | `void (ptr)` | Print length-prefixed bool array as JSON | None |
| `tk_json_print_f64` | `void (double)` | Print f64 as decimal number | None |
| `tk_json_print_arr_str` | `void (ptr, i64)` | Print string array as JSON (takes data ptr + len) | None |
| `tk_array_concat` | `ptr (ptr, ptr)` | Concatenate two length-prefixed i64 arrays | malloc -- caller owns result |
| `tk_str_concat` | `ptr (ptr, ptr)` | Concatenate two C strings | malloc -- caller owns result |
| `tk_str_len` | `i64 (ptr)` | Return byte length of a C string (`0` for NULL) | None |
| `tk_str_cmp` | `i64 (ptr, ptr)` | NULL-safe three-way string compare; backs `==` `!=` `<` `>` `<=` `>=` on `$str`. NULL is the `?(T)` miss sentinel, is **not** equal to `""`, and orders before every string | None |
| `tk_str_char_at` | `i64 (ptr, i64)` | Return char code at index (unsigned byte) | None |
| `tk_overflow_trap` | `void (i32)` | Print RT002 diagnostic and exit(1) | None (terminates) |

---

## 9. Runtime Traps

The emitted code fails **loudly at the fault site** rather than corrupting memory
or invoking undefined behaviour. Each trap prints `RTNNN: …` to stderr and
`exit(1)`; the `unreachable` after the call lets `-O2` delete the guard where it
can prove the fault is impossible.

| Code | Condition | Runtime fn | Message |
|------|-----------|-----------|---------|
| RT002 | `i64` add/sub/mul overflow | `tk_overflow_trap` | `RT002: integer overflow in <op>` |
| RT003 | array/collection index out of bounds | `tk_bounds_trap` | `RT003: index N out of bounds for length M` |
| RT004 | divide-by-zero / `INT64_MIN / -1` | `tk_div_trap` | `RT004: <division|remainder> by zero` |
| RT005 | nil struct-field dereference | `tk_nil_trap` | `RT005: nil dereference` |

(See [memory-model.md §6.6](spec/memory-model.md) for the normative spatial/
arithmetic-safety properties, and ADR-0012 for the design.)

### 9.1 Overflow (RT002)

Integer arithmetic on `i64` uses LLVM checked intrinsics:

| Operation | Intrinsic | op_code |
|-----------|-----------|---------|
| Addition  | `@llvm.sadd.with.overflow.i64` | 0 |
| Subtraction | `@llvm.ssub.with.overflow.i64` | 1 |
| Multiplication | `@llvm.smul.with.overflow.i64` | 2 |

**IR pattern**:
```llvm
%r = call {i64, i1} @llvm.sadd.with.overflow.i64(i64 %a, i64 %b)
%val = extractvalue {i64, i1} %r, 0
%ov  = extractvalue {i64, i1} %r, 1
br i1 %ov, label %ov_trap, label %ov_ok

ov_trap:
  call void @tk_overflow_trap(i32 0)
  unreachable

ov_ok:
  ; use %val
```

`tk_overflow_trap` prints to stderr and terminates:
```
RT002: integer overflow in addition
```

The process exits with code 1. The `unreachable` after the call allows
LLVM to optimize the non-overflow path.

---

## 10. Initialization

The emitted `main()` wrapper calls `tk_runtime_init(argc, argv)` before
any toke code executes.

`tk_runtime_init` stores `argc` and `argv` into file-scoped globals
(`g_argc`, `g_argv`) so that `tk_str_argv(index)` can access command-line
arguments at any point during execution.

- Out-of-bounds `tk_str_argv` calls return `""` (empty string, not NULL).
- `argv[0]` is the program name, `argv[1]` is typically the JSON input
  for benchmark programs.

`tk_runtime_init` also calls `tk_cap_init(argc, argv)` to initialise the
capability broker (see §12).

---

## 11. Injection Resistance

The runtime neutralises the injection classes closed in ADR-0011; these are
guarantees of the emitted/stdlib behaviour, not advice:

1. **Argv-only process exec.** `process.exec`/`spawn`/`spawndetached` never route
   through `sh -c`. The command string is tokenised on whitespace (quoted segments
   preserved) and `execvp`'d directly, so shell metacharacters (`;`, `|`, `$()`,
   backticks, `&&`) are treated literally — a command-injection payload runs no
   extra process.
2. **Context-aware HTML/template escaping (escape-by-default).** Interpolated
   values are HTML-escaped by default in both `tpl.render` (stdlib) and the ooke
   `{= =}` template engine; attribute values, and JS/CSS/`<title>` raw-text
   contexts, are escaped for their context; `md.render` runs with cmark's safe
   default (raw HTML in markdown source is neutralised). A `|raw` filter is the
   explicit, greppable opt-out for trusted HTML.
3. **URL-scheme neutralisation.** `javascript:`/`vbscript:`/`data:` URLs are dropped
   from `href`/`src`/`action`/… attributes.

(See [memory-model.md §6.7](spec/memory-model.md) and ADR-0011.)

---

## 12. At-Rest Crypto Envelope

`encrypt.seal`/`encrypt.open` wrap AEAD output in a self-describing, versioned,
algorithm-tagged envelope so ciphertext at rest is upgradeable without breaking
format:

```
[ 'T' 'K' 'E' | version(1) | alg(1) | nonce(12) | ciphertext | tag(16) ]
```

`open` dispatches on `(version, alg)` and rejects unknown/tampered values, so an
attacker cannot downgrade or confuse the format. `alg` 1 = AES-256-GCM, 2 =
ChaCha20-Poly1305 (both 256-bit AEADs). New algorithms (incl. PQC) are added as new
`alg` ids. TLS uses hybrid post-quantum key exchange (`X25519MLKEM768`, classical
fallback) and an opt-in ML-DSA-65 self-signed cert path. (See ADR-0013.)

---

## 13. Capability Broker

`tk_cap_init` (called from `tk_runtime_init`) computes the effective grant set from
the compiler-baked `@__tk_cap_baked_{grants,present,enforce}` globals unioned with
runtime `--allow-*` flags. Each fs/net/env/process sink calls `tk_cap_check`; a
denied call raises `CAP001` and exits before the syscall. Default mode is
`ALLOW_ALL` (no-op) until the deny-by-default flip. Full details in
[spec/capabilities.md](spec/capabilities.md) and ADR-0010.
