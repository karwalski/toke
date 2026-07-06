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

**Allocation**: stack-allocated via `alloca`:
```llvm
%t0 = alloca i64, i32 3  ; struct with 3 fields
```

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

**[TODO]** -- Error unions (`T!Err`) are defined in the type system
(`TY_ERROR_TYPE`) but do not yet have a codegen representation in
`llvm.c`.

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
| `tk_str_len` | `i64 (ptr)` | Return byte length of a C string | None |
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
