# Audit: Bitwise Operator Symbols `& ^ ~ % << >>`

**Story:** 77.1.7
**Date:** 2026-05-01
**Spec version:** toke-spec-v0.3

---

## Summary

All six bitwise operators plus modulo were added in v0.3 (spec section 11.15).
They are fully implemented in the compiler pipeline (lexer, parser, type checker,
LLVM IR emitter) but have **zero usage** in stdlib, tests, or examples.
The `&name` function reference syntax is specified (section 24.8) but **not
implemented** in the parser -- only the binary bitwise AND meaning of `&` exists
in code today.

---

## Per-Symbol Analysis

### `&` -- Bitwise AND (binary infix)

| Aspect | Status |
|--------|--------|
| Spec reference | S11.15: "Bitwise AND", LLVM IR `and`, Integer operands |
| Lexer | Implemented: single `&` emits `TK_AMP`; `&&` emits `TK_AND` (lexer.c:684-690) |
| Parser | Implemented: `parse_bitand()` at precedence level 5 (parser.c:665-668) |
| Type checker | Implemented: integer type enforcement (types.c:700) |
| LLVM codegen | Implemented: emits `and %ty %lhs, %rhs` (llvm.c:1576) |
| Type inference | Implemented: returns narrowest integer type (llvm.c:2636-2641) |
| Stdlib usage | None |
| Test coverage | None -- no bitwise-specific conformance, e2e, or unit test |
| Example usage | None |

### `&` -- Function Reference Prefix (unary prefix, `&name`)

| Aspect | Status |
|--------|--------|
| Spec reference | S24.8: "PROMOTED (limited)" -- `&name` for first-class function values |
| Compiler implementation | **NOT IMPLEMENTED** -- no `NODE_FUNC_REF` or equivalent AST node exists; `parse_primary()` does not handle `TK_AMP` at expression start |
| Disambiguation plan (spec) | LL(1) position-based: `&` at expression-start = function ref, `&` as continuation (after a value) = bitwise AND |
| Actual disambiguation | N/A -- only bitwise AND path exists in parser today |

**Key finding:** The spec says `&name` is "promoted (limited)" but the compiler
has no implementation. The `&` token in expression-start position would currently
cause a parse error (it falls through `parse_primary` without matching).

### `^` -- Bitwise XOR

| Aspect | Status |
|--------|--------|
| Spec reference | S11.15: "Bitwise XOR", LLVM IR `xor`, Integer operands |
| Lexer | Implemented: `TK_CARET` (lexer.c:691) |
| Parser | Implemented: `parse_bitxor()` at precedence level 6 (parser.c:672-676) |
| Type checker | Implemented (types.c:700) |
| LLVM codegen | Implemented: emits `xor %ty %lhs, %rhs` (llvm.c:1578) |
| Stdlib usage | None |
| Test coverage | None |
| Example usage | None |

### `~` -- Bitwise NOT (unary prefix)

| Aspect | Status |
|--------|--------|
| Spec reference | S11.15: "Bitwise NOT (unary)", LLVM IR `xor %v, -1`, Integer operands |
| Lexer | Implemented: `TK_TILDE` (lexer.c:692) |
| Parser | Implemented: handled in `parse_unary()` alongside `-` and `!` (parser.c:588-590) |
| Type checker | Implied integer (returns operand type) |
| LLVM codegen | Implemented: emits `xor %ty %v, -1` (llvm.c:1616-1618) |
| Type inference | Implemented: returns operand type (llvm.c:2648) |
| Stdlib usage | None |
| Test coverage | None |
| Example usage | None |

### `<<` -- Left Shift

| Aspect | Status |
|--------|--------|
| Spec reference | S11.15: "Left shift", LLVM IR `shl`, Integer operands |
| Lexer | Implemented: two-char `<<` emits `TK_SHL` (lexer.c:663-665) |
| Parser | Implemented: `parse_shift()` at precedence level 4 (parser.c:651-655) |
| Type checker | Implemented (types.c:701) |
| LLVM codegen | Implemented: emits `shl %ty %lhs, %rhs` (llvm.c:1579) |
| Stdlib usage | None |
| Test coverage | None |
| Example usage | None |

### `>>` -- Arithmetic Right Shift

| Aspect | Status |
|--------|--------|
| Spec reference | S11.15: "Arithmetic right shift", LLVM IR `ashr`, Integer operands |
| Lexer | Implemented: two-char `>>` emits `TK_SHR` (lexer.c:670-672) |
| Parser | Implemented: in `parse_shift()` alongside `<<` (parser.c:651-655) |
| Type checker | Implemented (types.c:701) |
| LLVM codegen | Implemented: emits `ashr %ty %lhs, %rhs` (llvm.c:1580) |
| Stdlib usage | None |
| Test coverage | None |
| Example usage | None |

### `%` -- Modulo (Remainder)

| Aspect | Status |
|--------|--------|
| Spec reference | S11.15: "Modulo (remainder)", LLVM IR `srem`, Integer operands |
| Lexer | Implemented: `TK_PERCENT` (lexer.c:693) |
| Parser | Implemented: grouped with `*` and `/` in `parse_mul()` at precedence level 2 (parser.c:610) |
| Type checker | Implemented (types.c:701) -- grouped with other bitwise ops, not arithmetic |
| LLVM codegen | Implemented: emits `srem %ty %lhs, %rhs` (llvm.c:1581) |
| Stdlib usage | None |
| Test coverage | None |
| Example usage | None |

**Note on `%` precedence:** The spec lists `%` at precedence level 2 (Multiplicative,
with `*` and `/`) and the parser correctly implements this -- `parse_mul()` handles
`TK_STAR`, `TK_SLASH`, and `TK_PERCENT` together. However, the type checker
groups `%` with the bitwise operators for type enforcement (integer-only), which
is correct since `%` does not apply to floats in toke.

---

## `&` Disambiguation: Deep Dive

**Spec intent:** `&` at expression-start means function reference; `&` after a
value (as binary continuation) means bitwise AND.

**LL(1) viability:** This is straightforward for an LL(1) parser because:
- `parse_primary()` handles expression-start tokens (identifiers, literals, `(`, `@`, `$`, `-`, `!`, `~`)
- `parse_bitand()` handles the binary `&` only after a left-hand operand has already been parsed
- Adding `TK_AMP` to `parse_primary()` (producing `NODE_FUNC_REF` when followed by `TK_IDENT`) would not conflict with the binary meaning

**If bitwise `&` were removed:**
- `&name` could still work as function reference with no ambiguity at all
- The `&&` (logical AND) token is already distinct (`TK_AND`) and unaffected
- Impact: programs using bitwise AND would break, but there are currently zero such programs

---

## Could They Be Deferred?

| Symbol | Remove impact | Recommendation |
|--------|-------------|----------------|
| `&` (bitwise) | Zero breakage -- no usage anywhere | Safe to defer; resolves `&name` ambiguity concern entirely |
| `^` | Zero breakage | Safe to defer |
| `~` | Zero breakage | Safe to defer |
| `<<` | Zero breakage | Safe to defer |
| `>>` | Zero breakage | Safe to defer |
| `%` | Zero breakage | **Keep** -- modulo is commonly needed and already implemented at the right precedence level |

**What breaks if all bitwise ops are removed:**
- Nothing in stdlib, tests, examples, or any known toke source file
- The lexer tokens, parser rules, type checker paths, and LLVM codegen for these operators would become dead code
- The character set count (59) would need to be recounted if symbols are removed from the language

**Recommended action:**
1. `%` (modulo): Keep -- it is arithmetic, commonly needed, correctly placed
2. `& ^ ~ << >>` (pure bitwise): Defer to v0.5+ unless a concrete use case emerges
3. `&name` (function ref): Implement in parser.c `parse_primary()` as a priority -- this is spec'd as "promoted" and is needed for callback patterns (HTTP handlers, etc.)

---

## File References

- Spec: `/Users/matthew.watt/tk/docs/spec/toke-spec-v0.3.md` (lines 1021-1051, 2776-2780)
- Lexer: `/Users/matthew.watt/tk/toke/src/lexer.c` (lines 663-693)
- Lexer tokens: `/Users/matthew.watt/tk/toke/src/lexer.h` (lines 63-68)
- Parser: `/Users/matthew.watt/tk/toke/src/parser.c` (lines 588-690)
- Type checker: `/Users/matthew.watt/tk/toke/src/types.c` (line 700)
- LLVM codegen: `/Users/matthew.watt/tk/toke/src/llvm.c` (lines 1576-1581, 1616-1618, 2636-2648)
