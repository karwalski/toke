# Audit: Comparison and Logic Symbols

**Story:** 77.1.3
**Date:** 2026-05-01
**Spec version:** toke-spec-v0.3
**Compiler source:** toke/src/{lexer.c, parser.c, llvm.c}

---

## Summary Table

| Symbol | Token(s) | Uses | Overloaded? |
|--------|----------|------|-------------|
| `<` | TK_LT, TK_SHL (`<<`) | Return shorthand, less-than comparison, left-shift (as `<<`) | Yes (2 distinct roles for `<`) |
| `>` | TK_GT, TK_SHR (`>>`) | Greater-than comparison, right-shift (as `>>`) | No (single-char is one role only) |
| `!` | TK_BANG | Error propagation (postfix), logical NOT (prefix) | Yes (2 distinct roles) |
| `\|` | TK_PIPE, TK_OR (`\|\|`) | Bitwise OR, match block opener, union type separator | Yes (3 distinct roles for single `\|`) |
| `&&` | TK_AND | Logical AND (short-circuiting) | No |
| `\|\|` | TK_OR | Logical OR (short-circuiting) | No |
| `&` | TK_AMP | Bitwise AND, function reference prefix (spec; not yet in parser) | Partially (1 implemented, 1 deferred) |

---

## 1. `<` (Less-Than / Return Shorthand)

### 1.1 Spec Uses (toke-spec-v0.3)

1. **Return shorthand** (S11.4, S8.8): `<expr;` returns from a function. Preferred in single-expression bodies.
   - Grammar: `ReturnStmt = '<' Expr ';' | 'rt' Expr ';' ;`
2. **Less-than comparison** (S11.15): Binary infix operator between two numeric expressions.
   - Grammar: `CompareExpr = ShiftExpr ( ( '<' | '>' ) ShiftExpr )? ;`
3. **Left-shift** (as `<<`, TK_SHL): Two-char operator, lexed as a single TK_SHL token by longest-match.

### 1.2 Compiler Implementation

- **Lexer** (lexer.c:662-668): Checks if next char is also `<`; if so emits TK_SHL (2-char). Otherwise emits single TK_LT.
- **Parser** (parser.c:959): In `parse_stmt()`, TK_LT at **statement-start position** dispatches to NODE_RETURN_STMT. In expression context (`parse_compare`, parser.c:661), TK_LT produces NODE_BINARY_EXPR with op=TK_LT.
- **LLVM** (llvm.c:1573-1574): Integer comparison emits `icmp slt`; float comparison emits `fcmp olt`.

### 1.3 Stdlib / Examples

- `test/e2e/e2e_if.tk`: `<1` and `<0` as return statements.
- `test/e2e/e2e_logical.tk`: `<0` and `<1` as returns.
- Spec examples: `lp(let i=0;i<arr.len;i=i+1)` uses `<` as comparison.

### 1.4 LL(1) Disambiguation

The parser resolves the overload at **statement dispatch level**:

- **Statement-start**: `parse_stmt()` switches on `peek(p)`. If the leading token is TK_LT, it unconditionally parses a ReturnStmt (`<expr;`).
- **Expression context**: TK_LT never appears at statement-start when it means comparison, because an expression statement always starts with an IDENT, literal, `(`, or unary operator -- never with `<`.

This is pure LL(1): the single lookahead token at statement boundary determines the interpretation. No ambiguity exists because `< expr` at statement start is always return (there is no left-hand operand for a comparison to apply to).

### 1.5 Simplification Potential

**Low.** The `<` return shorthand is a core design choice for token reduction. Removing it would require `rt` everywhere, adding 1 token per return. The disambiguation is trivial and well-defined. No change recommended.

---

## 2. `>` (Greater-Than)

### 2.1 Spec Uses

1. **Greater-than comparison** (S11.15): Binary infix between numeric expressions.
2. **Right-shift** (as `>>`, TK_SHR): Two-char operator, lexed as single token.

### 2.2 Compiler Implementation

- **Lexer** (lexer.c:669-675): Checks next char for `>`; if so emits TK_SHR. Otherwise emits TK_GT.
- **Parser** (parser.c:661): `parse_compare()` handles TK_GT as binary comparison.
- **LLVM** (llvm.c:1574): Integer emits `icmp sgt`; float emits `fcmp ogt`.

### 2.3 Stdlib / Examples

- `test/e2e/e2e_if.tk`: `if(x>3)` as comparison.
- Spec: `if(a>0 && b>0)`.

### 2.4 LL(1) Disambiguation

**Not overloaded** as a single character (only one role in expressions). The `>>` two-char form is handled by longest-match in the lexer, so the parser never sees ambiguity.

### 2.5 Simplification Potential

**None.** Single unambiguous role. No change needed.

---

## 3. `!` (Error Propagation / Logical NOT)

### 3.1 Spec Uses

1. **Error propagation (postfix)** (S13.3): `expr!$errorvariant` -- propagates error if expr is `$err`, continues with value if `$ok`.
   - Grammar: `PropagateExpr = CallExpr ( '!' TypeExpr )? ;`
   - Also in type signatures: `f=name():$type!$errtype{...}`
2. **Logical NOT (prefix)** (S11.15): `!expr` -- boolean negation.
   - Grammar: `UnaryExpr = ('-' | '!' | '~') UnaryExpr | CastPropExpr`

### 3.2 Compiler Implementation

- **Lexer** (lexer.c:676): Always emits TK_BANG. No multi-char variants.
- **Parser**:
  - **Prefix NOT** (parser.c:590): `parse_unary()` checks if `peek(p)==TK_BANG` at the start of a unary expression. If so, consumes it and recurses, producing NODE_UNARY_EXPR with op=TK_BANG.
  - **Postfix propagation** (parser.c:567): `parse_cast_prop()` checks if `peek(p)==TK_BANG` AFTER parsing a CallExpr. If so, consumes it and parses a TypeExpr, producing NODE_PROPAGATE_EXPR.
  - **Return type signature** (parser.c:1289): In function signature parsing, `!` separates the return type from the error type.
- **LLVM**:
  - Logical NOT (llvm.c:1599-1614): `xor i1 %v, 1` (with i64->i1 coercion if needed).
  - Error propagation: handled via the result/error codegen path (NODE_PROPAGATE_EXPR).

### 3.3 Stdlib / Examples

- `test/e2e/e2e_logical.tk`: `if(!b){...}` uses prefix NOT.
- Spec: `db.one(sql;@(id))!$usererr.$db_err` uses postfix propagation.
- All fallible stdlib functions use `!` in signatures: `str.slice(...):str!SliceErr`.

### 3.4 LL(1) Disambiguation

**Precedence-based (no lookahead needed):**

- Prefix `!` is parsed at the **UnaryExpr** level (high precedence, top of descent).
- Postfix `!` is parsed at the **CastPropExpr** level (after a CallExpr has been fully consumed).

The parser descends `parse_or -> parse_and -> ... -> parse_unary -> parse_cast_prop -> parse_call`. By the time `parse_cast_prop` checks for `!`, the left operand (a CallExpr) has already been fully parsed. There is no ambiguity because:
- At UnaryExpr level: `!` appears with NO left-hand operand (pure prefix position).
- At CastPropExpr level: `!` appears AFTER a fully-parsed call expression (postfix position).

This is deterministic LL(1) since the parser knows which production it is in.

### 3.5 Simplification Potential

**Low.** Both uses are essential and well-established in the language. The prefix/postfix split is elegant and mirrors operator precedence naturally. The `!` in type signatures (`!$errtype`) is syntactically distinct (appears after `:$type` in a declaration context). No change recommended.

---

## 4. `|` (Pipe -- Match / Bitwise OR / Union Type)

### 4.1 Spec Uses

1. **Bitwise OR** (S11.15): Binary infix between integer expressions.
   - Grammar: `BitOrExpr = BitXorExpr ( '|' BitXorExpr )* ;`
2. **Match block opener** (S11.10): `expr|{arm1;arm2}` -- opens a pattern match.
   - Grammar: `MatchExpr = BitOrExpr ( '|' '{' MatchArmList '}' )? ;`
3. **Union type separator** (S12): In type declarations, separates union variants.
   - Spec S11.15: "In type declarations: union type separator"

### 4.2 Compiler Implementation

- **Lexer** (lexer.c:677-683): If next char is `|`, emits TK_OR (2-char `||`). Otherwise emits TK_PIPE (single `|`).
- **Parser**:
  - **Bitwise OR** (parser.c:682): `parse_bitor()` loops while `peek(p)==TK_PIPE && peek_at(p,1)!=TK_LBRACE`. The second condition (`peek_at(p,1)!=TK_LBRACE`) is the disambiguation -- if `|` is followed by `{`, it is a match block, not bitwise OR.
  - **Match block** (parser.c:763-769): `parse_expr()` checks `if(peek(p)!=TK_PIPE) return l;` then consumes `|` and expects `{` to begin match arms.
  - **Union type separator**: Not currently implemented in parser (type system union types are specified but likely handled at a higher semantic level or deferred).
- **LLVM** (llvm.c:1577): Bitwise OR emits `or %type %lhs, %rhs`.

### 4.3 Stdlib / Examples

- Spec: `getuser(id)|{ $ok:u <$res.ok(json.enc(u)); $err:e <$res.err(json.enc(e)) }` -- match block.
- Spec: `"type"|"func"|"const"` in module interface docs (informative, describes string union).
- No bitwise OR examples in e2e tests currently.

### 4.4 LL(1) Disambiguation

**1-token lookahead (LL(2) technically, but implemented as peek_at):**

The parser uses `peek_at(p, 1)` -- looking one token ahead of the current `|`:

- `|` followed by `{` (TK_LBRACE) = **match block opener**
- `|` followed by anything else = **bitwise OR**

This is explicitly noted in the spec grammar comment: *"'|' in MatchExpr is followed by '{'; in BitOrExpr it is followed by an expression. This requires one token of lookahead: '|' '{' begins a match block, '|' <expr> is bitwise OR."*

Strictly, this is LL(2) (needs 2 tokens: the `|` itself plus the next token). However, since the `|` is already consumed when making the decision, it functions as LL(1) from the perspective of the `|` being the "current" token and checking what follows.

**Union type separator**: Disambiguated by parser context -- type expressions are parsed in a different production (`parse_type_expr`) that is only invoked from declaration/signature positions, never from expression positions. No runtime ambiguity.

### 4.5 Simplification Potential

**Medium consideration, but no change recommended.** The triple overload is the most complex disambiguation in toke. However:
- The LL(2) lookahead for match vs. bitwise OR is well-defined and simple to implement.
- Union types are in declaration context only, completely separate from expression parsing.
- Removing any use would require introducing a new symbol (reducing the benefit of a small character set).
- The match syntax `expr|{...}` is compact and intentional for token reduction.

**Risk note:** If future syntax adds `|` in other positions (e.g., closures), disambiguation would become more complex. The current design is stable.

---

## 5. `&&` (Logical AND)

### 5.1 Spec Uses

1. **Logical AND** (S11.9): Short-circuiting boolean operator.
   - Grammar: `LogAndExpr = MatchExpr ( '&&' MatchExpr )* ;`

### 5.2 Compiler Implementation

- **Lexer** (lexer.c:684-689): If `&` followed by `&`, emits TK_AND (2-char). Otherwise emits TK_AMP (single `&`).
- **Parser** (parser.c:689): `parse_and()` loops while `peek(p)==TK_AND`, producing NODE_BINARY_EXPR with op=TK_AND.
- **LLVM** (llvm.c:1359-1408): Short-circuit codegen using branching:
  - Allocates result (defaults to `false`).
  - Evaluates LHS; if false, skips RHS (branches to end).
  - If LHS true, evaluates RHS and stores result.

### 5.3 Stdlib / Examples

- `test/e2e/e2e_logical.tk`: `let a = true && true;`, `let e = true || false && false;`
- Spec: `if(a>0 && b>0){...}`

### 5.4 LL(1) Disambiguation

**Not overloaded.** `&&` is always logical AND. It is lexed as a single 2-char token (TK_AND) distinct from single `&` (TK_AMP = bitwise AND). The lexer's longest-match rule handles this: `&` followed by `&` always produces TK_AND, never two TK_AMP tokens.

### 5.5 Relationship to `&` (Bitwise AND)

`&` (TK_AMP) and `&&` (TK_AND) are **completely distinct tokens** after lexing:
- `&` = bitwise AND (integer operation, no short-circuit)
- `&&` = logical AND (boolean operation, short-circuit)

They occupy different precedence levels:
- `&` (bitwise AND): precedence 5
- `&&` (logical AND): precedence 10

No ambiguity exists. The lexer resolves `&&` vs `&` by longest-match before the parser ever sees them.

### 5.6 Simplification Potential

**None.** Standard, unambiguous, well-understood semantics. No change needed.

---

## 6. `||` (Logical OR)

### 6.1 Spec Uses

1. **Logical OR** (S11.9): Short-circuiting boolean operator.
   - Grammar: `LogOrExpr = LogAndExpr ( '||' LogAndExpr )* ;`

### 6.2 Compiler Implementation

- **Lexer** (lexer.c:677-683): If `|` followed by `|`, emits TK_OR (2-char). Otherwise emits TK_PIPE (single `|`).
- **Parser** (parser.c:696): `parse_or()` loops while `peek(p)==TK_OR`, producing NODE_BINARY_EXPR with op=TK_OR.
- **LLVM** (llvm.c:1359-1408): Short-circuit codegen (same pattern as `&&` but inverted -- defaults to `true`, skips RHS if LHS is true).

### 6.3 Stdlib / Examples

- `test/e2e/e2e_logical.tk`: `let c = false || true;`
- Spec: implicit in LogOrExpr grammar.

### 6.4 LL(1) Disambiguation

**Not overloaded.** `||` is always logical OR. Lexed as single 2-char token (TK_OR) distinct from single `|` (TK_PIPE). Longest-match in lexer resolves completely.

### 6.5 Relationship to `|` (Bitwise OR / Match / Union)

`|` (TK_PIPE) and `||` (TK_OR) are **completely distinct tokens** after lexing:
- `|` = bitwise OR / match block opener / union separator
- `||` = logical OR (boolean, short-circuit)

Precedence levels:
- `|` (bitwise OR): precedence 7
- `||` (logical OR): precedence 11

No ambiguity. Lexer resolves by longest-match.

### 6.6 Simplification Potential

**None.** Standard, unambiguous. No change needed.

---

## 7. `&` (Bitwise AND / Function Reference)

### 7.1 Spec Uses

1. **Bitwise AND** (S11.15): Binary infix between integer expressions.
   - Grammar: `BitAndExpr = EqExpr ( '&' EqExpr )* ;`
2. **Function reference prefix** (S24.8, promoted limited): `&name` creates a function reference value.
   - Status: "PROMOTED (limited)" in spec v0.3. Targeted for v0.4 full closures.

### 7.2 Compiler Implementation

- **Lexer** (lexer.c:684-690): If followed by `&`, emits TK_AND. Otherwise emits TK_AMP.
- **Parser** (parser.c:668): `parse_bitand()` handles TK_AMP as binary bitwise AND.
- **Function reference**: NOT currently implemented in the parser. No NODE_FUNC_REF exists in the codebase.
- **LLVM** (llvm.c:1576): Bitwise AND emits `and %type %lhs, %rhs`.

### 7.3 LL(1) Disambiguation (Future)

When `&name` is implemented, disambiguation will follow the same pattern as `!`:
- **Prefix `&`** (at expression-start, followed by IDENT): function reference.
- **Infix `&`** (between two expressions): bitwise AND.

This is naturally LL(1) because in infix position there is always a left operand already parsed; at expression-start (unary position) there is no left operand. The same precedence-descent mechanism used for `!` (prefix NOT vs postfix propagation) applies.

### 7.4 Simplification Potential

**None currently.** Only one role is implemented (bitwise AND). When function references are added, the prefix/infix distinction is trivial. No change needed.

---

## 8. Overall Disambiguation Summary

| Symbol | Mechanism | LL(k) | Complexity |
|--------|-----------|-------|------------|
| `<` (return vs comparison) | Statement-start dispatch | LL(1) | Trivial |
| `>` | No overload | LL(1) | None |
| `!` (prefix NOT vs postfix propagate) | Precedence descent position | LL(1) | Low |
| `\|` (bitwise OR vs match block) | peek_at(1) for `{` | LL(2)* | Low |
| `\|` (expr vs type union) | Parser production context | LL(1) | None |
| `&&` vs `&` | Lexer longest-match | LL(1) | None |
| `\|\|` vs `\|` | Lexer longest-match | LL(1) | None |

*Technically LL(2) but implemented trivially with one-token lookahead past the current symbol.

---

## 9. Recommendations

1. **No symbols should be removed.** Each serves essential roles, and the character set is intentionally minimal.
2. **No simplification opportunities identified.** All disambiguations are well-defined, implemented correctly, and impose minimal parser complexity.
3. **Future risk: `&` prefix for function references** -- when implemented, ensure the unary/binary distinction mirrors the `!` pattern (precedence descent). This is straightforward.
4. **Future risk: `|` in closures** -- if closures use `|params|` syntax (as in Rust), this would conflict with match and bitwise OR. The spec currently defers closures (v0.4) and does not specify pipe-delimited parameters, so no current risk.
5. **Documentation note:** The `|` three-way overload is the most complex disambiguation point. The spec comment at the grammar level (lines 640-642) correctly documents this. The parser implementation at `parse_bitor()` with `peek_at(p,1)!=TK_LBRACE` is correct and minimal.
