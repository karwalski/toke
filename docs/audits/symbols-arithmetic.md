# Audit: Arithmetic Operator Symbols `+ - * /`

**Story:** 77.1.2
**Date:** 2026-05-01
**Spec version:** toke-spec-v0.3

---

## Summary Table

| Symbol | Primary use | Secondary use | Removable? |
|--------|-------------|---------------|------------|
| `+` | Integer/float addition | String concat, array concat | No |
| `-` | Integer/float subtraction | Unary negation | No |
| `*` | Integer/float multiplication | Pointer type prefix (FFI only) | No |
| `/` | Integer/float division | (none) | No |

---

## `+` — Addition / Concatenation

### Spec references

- Section 11.15 precedence table: additive tier (level 3)
- Appendix D: `+` = "add" (operator), "string concat" (delimiter column)
- Section 16 stdlib: `str.concat(a:str;b:str):str` is the function form

### Compiler implementation

**Lexer** (`src/lexer.c:658`): Single character `+` produces `TK_PLUS`.

**Parser** (`src/parser.c:621-630`): Parsed at the additive precedence level. Creates `NODE_BINARY_EXPR` with `op = TK_PLUS`. Left-associative, lower precedence than `*`/`/`/`%`.

**LLVM codegen** (`src/llvm.c:1413-1434`):
- If both operands are `i8*` (string/array pointers): emits `call i8* @tk_array_concat(...)` — handles both string and array concatenation via the same runtime function.
- If one operand is `i8*` and the other `i64`: coerces the i64 side to pointer, then calls `tk_array_concat` (supports mixed `arr + [x]` patterns).
- For `f32`/`f64`: emits `fadd`.
- For `i64`: emits checked arithmetic via `@llvm.sadd.with.overflow.i64` with overflow trap (RT003).
- For narrow integers (`i8`/`i16`/`i32`): emits plain `add` (wraps).

### Stdlib usage

- `stdlib/path.tk`: `i=i+1` (loop counter increment), `pos+1` (index arithmetic)
- `stdlib/encoding.tk`: `idx+65`, `outlen+4`, `bi+1`, `oi+4` (buffer offset arithmetic)
- `stdlib/toon.tk`: `bracepos+1`, `closepos+1`, `p+1`, `p+2` (parser position arithmetic)

### Dual uses

1. **Integer addition** — standard arithmetic on all integer types
2. **Float addition** — IEEE 754 addition on f32/f64
3. **String concatenation** — `"hello" + " world"` compiles to `tk_array_concat`
4. **Array concatenation** — `@(1;2) + @(3;4)` compiles to `tk_array_concat`

The string/array overload is implemented entirely in the LLVM backend by inspecting operand types — the parser does not distinguish these cases.

### Could it be removed?

**No.** Addition is fundamental. The concatenation overload could theoretically be replaced by mandating `str.concat()` calls, but `+` for concat is documented in the spec (Appendix D) and is used pervasively in stdlib `.tk` implementations. Removing would break all string/array concatenation expressions.

---

## `-` — Subtraction / Unary Negation

### Spec references

- Section 11.15 precedence table: unary tier (level 1) for negation, additive tier (level 3) for subtraction
- Appendix D: `-` = "subtract" (operator), "unary negate" (delimiter column)

### Compiler implementation

**Lexer** (`src/lexer.c:659`): Single character `-` produces `TK_MINUS`.

**Parser** (`src/parser.c:578-590`): As unary prefix, parsed at unary precedence level. Creates `NODE_UNARY_EXPR` with `op = TK_MINUS`. As binary, parsed at additive level alongside `+`.

**LLVM codegen**:
- Binary subtraction (`src/llvm.c:1540`): For i64, uses `@llvm.ssub.with.overflow.i64` with trap. For narrow ints, emits `sub`. For floats, emits `fsub`.
- Unary negation (`src/llvm.c:1592-1597`): For floats, emits `fneg`. For integers, emits `sub <type> 0, %v`.

### Stdlib usage

- `stdlib/encoding.tk`: `idx-26`, `idx-52`, `b-65`, `b-97`, `b-48` (character code arithmetic)
- `stdlib/path.tk`: `pos-1` (index decrement)

### Dual uses

1. **Binary subtraction** — integer and float subtraction
2. **Unary negation** — prefix minus (`-x` becomes `sub 0, x` or `fneg`)

The parser disambiguates by position: if `-` appears where an expression is expected (after an operator, at statement start, after `(`), it is unary; otherwise binary.

### Could it be removed?

**No.** Both subtraction and negation are essential. Separating them into different symbols would be non-standard and reduce readability. No language successfully removes either use.

---

## `*` — Multiplication / Pointer Type (FFI)

### Spec references

- Section 11.15 precedence table: multiplicative tier (level 2)
- Appendix D: `*` = "multiply" (operator), "pointer deref (FFI only)" (delimiter column)
- Section 14.4 "No Pointer Arithmetic": raw pointers only accessible through FFI declarations
- Section 24.2: FFI is deferred but pointer types exist for FFI function signatures

### Compiler implementation

**Lexer** (`src/lexer.c:660`): Single character `*` produces `TK_STAR`.

**Parser**:
- As type prefix (`src/parser.c:311`): `if(peek(p)==TK_STAR)` in `parse_type_expr()` — creates `NODE_PTR_TYPE`. This is **not** pointer dereference; it is pointer **type annotation** (e.g., `*u8` meaning "pointer to u8").
- As multiplication (`src/parser.c:601-610`): In expression context at multiplicative precedence. Creates `NODE_BINARY_EXPR` with `op = TK_STAR`.

**LLVM codegen** (`src/llvm.c:1535-1565`):
- For i64: uses `@llvm.smul.with.overflow.i64` with trap.
- For narrow ints: emits `mul`.
- For floats: emits `fmul`.

### Stdlib usage

- `stdlib/encoding.tk`: `fullgroups*4`, `gi*3`, `fullgroups*3` (buffer size calculations)

### Dual uses

1. **Binary multiplication** — integer and float multiplication
2. **Pointer type prefix** — `*u8`, `*i64` in FFI function parameter/return types

**Important finding:** The spec says "pointer deref (FFI only)" in Appendix D, but the compiler does NOT implement pointer dereference as an expression operator. The `*` in type position is a **type constructor** (like `@` for arrays), not a dereference operator. There is no `*ptr` dereference expression in the parser — only `*Type` in type annotations.

Evidence from test files:
- `test/grammar/G029.yaml`: `f=memcpy(dst:*u8;src:*u8;n:i64):*u8` — pointer type in FFI signature
- `test/grammar/G027.yaml`: `f=puts(s:*u8):i32` — pointer type in FFI signature
- `test/diagnostics/D013.yaml`: `f=bad(s:*u8):i64{<42}` — pointer type in function signature

No test or stdlib file uses `*expr` as a dereference expression. The Appendix D description "pointer deref" is slightly misleading — it is more accurately "pointer type prefix".

### Was this legacy only?

The `*` pointer type exists in both default and legacy syntax. It was never "removed" from default. However, pointer **dereference** (using `*` as a unary prefix on a value expression to read through a pointer) does not exist in either profile. The spec at 14.4 explicitly states: "Pointer arithmetic is not defined... all memory access is through named fields, array `.get()` calls, and function calls."

### Could it be removed?

**Multiplication: No.** Fundamental arithmetic operation.

**Pointer type prefix: Could be replaced** but would require an alternative syntax for FFI signatures. Since FFI is deferred (Section 24.2) and pointer types are only used in extern function declarations, this is low-impact but would break existing FFI test cases and any external code declaring C bindings.

---

## `/` — Division

### Spec references

- Section 11.15 precedence table: multiplicative tier (level 2)
- Appendix D: `/` = "divide" (operator), "--" (no secondary use)

### Compiler implementation

**Lexer** (`src/lexer.c:661`): Single character `/` produces `TK_SLASH`.

**Parser** (`src/parser.c:601-610`): Parsed at multiplicative precedence level alongside `*` and `%`. Creates `NODE_BINARY_EXPR` with `op = TK_SLASH`.

**LLVM codegen** (`src/llvm.c:1572`): Emits `sdiv` for integers. For floats, emits `fdiv`. Division does NOT use overflow intrinsics (division overflow is only possible for `MIN_INT / -1`, which is undefined behavior in LLVM IR — the spec does not mention special handling for this case).

### Stdlib usage

- `stdlib/encoding.tk`: `inlen/3` (calculating number of full base64 groups)

### Dual uses

**None.** `/` is the only arithmetic operator with no dual use in toke. It is not used for comments (toke uses `(* ... *)`), not used for path separators, and not used for regex delimiters.

### Could it be removed?

**No.** Division is fundamental arithmetic. There is no alternative syntax for division in the language.

---

## Precedence Summary (arithmetic subset)

```
Level 1 (highest): Unary  -  ~  !
Level 2:           * / %
Level 3:           + -
```

All arithmetic operators are left-associative. Parentheses `()` override precedence.

---

## Key Findings

1. **`+` has the most overloads** (4 uses: int add, float add, string concat, array concat) but the overload is type-directed and unambiguous at compile time.

2. **`*` pointer dereference does not actually exist** as an expression operator. The spec's Appendix D entry is misleading. `*` in type position is a type constructor, not a dereference. Consider updating Appendix D to say "pointer type prefix (FFI only)" instead of "pointer deref (FFI only)".

3. **`/` is the cleanest symbol** — single use, no overloads, no ambiguity.

4. **Division overflow** (`MIN_INT / -1`) is not handled with checked arithmetic intrinsics, unlike `+`, `-`, and `*` which all use overflow traps for i64. This may be a gap worth tracking.

5. **No arithmetic operator can be removed** without breaking fundamental language functionality. The pointer type prefix on `*` is the most "removable" secondary use, but it would require an FFI syntax redesign.
