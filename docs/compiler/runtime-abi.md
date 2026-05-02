---
title: Toke Runtime ABI v0
slug: runtime-abi
section: compiler
order: 5
---

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

## 9. Overflow Trap

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
