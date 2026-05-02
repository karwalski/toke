# Audit: The `@` Sigil in Toke

**Story:** 77.1.5
**Date:** 2025-05-01
**Scope:** Spec v0.3, compiler (lexer + parser), stdlib (.tki + docs), examples

---

## 1. Summary of Roles

The `@` character serves exactly **two grammatical roles** in toke default syntax, both related to collections:

| Role | Syntax | Example | Spec reference |
|------|--------|---------|----------------|
| Array type | `@TypeExpr` | `@i64`, `@$str`, `@$payment` | Spec line 693, Section on types |
| Map type | `@(TypeExpr:TypeExpr)` | `@(i64:str)`, `@($str:$str)` | Spec line 694 |
| Array literal | `@(expr; expr; ...)` | `@(1;2;3)`, `@()` | Spec line 671 |
| Map literal | `@(expr:expr; ...)` | `@("key":"val")`, `@(1:10;2:20)` | Spec line 674 |

Additionally, `mut.@()` is the idiomatic pattern for creating a mutable empty array.

---

## 2. Lexer Handling

File: `/Users/matthew.watt/tk/toke/src/lexer.c` (line 701-707)

- `@` emits a single `TK_AT` token in **default mode only**.
- In **legacy mode** (`PROFILE_LEGACY`), `@` triggers `E1003` ("character outside Profile 1 character set") and halts lexing.
- The lexer does NOT distinguish between `@` used for types vs. literals; that disambiguation happens in the parser.

---

## 3. Parser Handling

File: `/Users/matthew.watt/tk/toke/src/parser.c`

### 3.1 Type position (line 316-323)

```c
/* @$type for array type, @($key:$val) for map type */
if(peek(p)==TK_AT){
    Token *at=adv(p);
    if(peek(p)==TK_LPAREN){  // @(K:V) = map type
        ...NODE_MAP_TYPE...
    }
    // else: @T = array type
    ...NODE_ARRAY_TYPE...
}
```

Disambiguation rule: `@(` in a **type** context is always a map type. A bare `@` followed by a non-paren type expression is an array type.

### 3.2 Expression position (line 433-449)

```c
/* @(...) array/map literal */
if(peek(p)==TK_AT){
    Token *at=adv(p);
    xp(p,TK_LPAREN,"'(' after '@'");  // ALWAYS requires '('
    if(peek(p)==TK_RPAREN){...}  // empty @()
    Node *first=parse_expr(p);
    if(peek(p)==TK_COLON){  // MapLit: @(key:val; ...)
        ...NODE_MAP_LIT...
    }
    // else: ArrayLit: @(expr; expr; ...)
    ...NODE_ARRAY_LIT...
}
```

Disambiguation rule: `@(` in an **expression** context parses the first element, then checks for `:` to decide array vs. map.

---

## 4. Consistency Analysis

### 4.1 Is `@` for array type consistent?

**Yes.** The pattern `@T` always means "array of T" in type position. Examples from the codebase:

- `@i64` (spec, examples)
- `@$str` (stdlib/toon.tk, examples)
- `@$payment` (examples/mortgage)
- `@f64` (examples/mortgage-web)
- `@bool` (examples/datapipe)
- `@$bookmark` (examples/bookmarks-api)
- `@byte` (stdlib/encrypt.md)
- `@(f32)` (stdlib/vecstore) -- this is actually `@(f32)` meaning array-of-f32 using parens, BUT in type position `@(` triggers the **map type** parser path.

### 4.2 Ambiguity: `@(f32)` in stdlib

**FINDING:** Several stdlib files use `@(f32)` to mean "array of f32":
- `vecstore.tki` line 24: `"type": "@(f32)"`
- `vecstore.tki` line 50-51: `"@(f32)"` in params
- Spec line 1810, 1817, 1818, 1882, 1895: `@(f32)` as "array of f32"

However, in the parser grammar, `@(TypeExpr:TypeExpr)` is a map type, while `@TypeExpr` is an array type. The syntax `@(f32)` with a single type inside parens does NOT match either production -- it lacks the `:` for a map type and has parens that an array type does not expect.

**This appears to be a spec/documentation inconsistency.** The formal grammar (line 693) says `ArrayTypeExpr = '@' TypeExpr` (no parens), but the prose and stdlib use `@(f32)` to mean array-of-f32.

Possible explanations:
1. The parens in `@(f32)` in .tki JSON strings are informal notation (the .tki files are metadata, not parsed toke source).
2. The spec prose at line 1079 uses `@$t` (no parens) which is correct grammar.

**Verdict:** The formal grammar is consistent. The informal `@(f32)` notation in .tki JSON and spec prose sections (stdlib signatures) is a documentation shorthand, not actual toke syntax. Actual toke code uses `@f32` for the type.

### 4.3 Array literal vs. array type -- same sigil, different context?

**Yes, but unambiguous.** The parser resolves this by context:
- **Type position** (after `:` in parameter declarations, return types, struct fields): `@` starts a type expression.
- **Expression position** (right-hand side, function arguments): `@(` starts a literal.

There is no syntactic ambiguity because:
- Array **literals** always require `@(...)` with parens.
- Array **types** are `@T` without parens (or `@(K:V)` for maps, which requires `:`).

### 4.4 Map type vs. map literal disambiguation

| Context | Syntax | Meaning |
|---------|--------|---------|
| Type | `@(i64:str)` | map type: keys are i64, values are str |
| Expr | `@(1:"a";2:"b")` | map literal: entries 1->"a", 2->"b" |

No ambiguity: the parser knows which context it is in.

### 4.5 Empty collection: `@()`

- In expression position: always produces `NODE_ARRAY_LIT` (an empty array).
- There is no way to express an empty map literal distinctly from an empty array literal via syntax alone. The type system must infer it from context.

---

## 5. Legacy Mode Comparison

In legacy mode (`--legacy`), `@` is forbidden. Instead:
- Array type: `[i64]`
- Map type: `[i64:str]`
- Array literal: `[1; 2; 3]`
- Map literal: `[1:10; 2:20]`

The parser supports `[]` syntax when `PROFILE_LEGACY` is active (lines 452-464 for literals, lines 324-326 for types).

---

## 6. Could the Language Work Without `@`?

**Technically yes** -- `[]` does the same job in legacy mode. The design choice to use `@` was deliberate:

1. **Character budget:** Default syntax has 56 characters. `[` and `]` were explicitly excluded from default mode (spec line 286: "Square brackets `[` `]` -- not used; array literals use `@(...)`, array types use `@$type`"). The `@` sigil reuses one character instead of two.

2. **Visual distinctness:** `@` is visually distinct from parentheses/braces, making collection types easy to spot in dense toke code.

3. **Token efficiency:** A single `@` character signals "collection follows" vs. two bracket characters.

4. **No conflict:** `@` has no other meaning in the language (unlike `$` which marks types, or `!` which marks error propagation).

---

## 7. Completeness Check: All `@` Contexts in Real Code

| Pattern | Meaning | Found in |
|---------|---------|----------|
| `@i64` | array of i64 type | spec, examples |
| `@$str` | array of str type | stdlib, examples |
| `@$bookmark` | array of struct type | examples |
| `@(str:str)` | map type str->str | examples |
| `@($str:$str)` | map type (with $ prefix) | stdlib |
| `@(1;2;3)` | array literal | spec, examples |
| `@()` | empty array literal | very common |
| `@("k":"v")` | map literal | examples |
| `@("k":expr;...)` | map literal (multi-entry) | examples |
| `mut.@()` | mutable empty array | 17 occurrences in codebase |
| `@(expr)` | single-element array | examples |

---

## 8. Potential Issues / Observations

1. **`@(f32)` notation in .tki files:** Inconsistent with formal grammar. Should be `@f32` in actual toke code. The .tki JSON files use a looser notation. Not a bug per se, but could confuse readers cross-referencing spec grammar with stdlib definitions.

2. **Empty map vs. empty array:** Both are `@()`. The type system must disambiguate. This is documented implicitly but could benefit from an explicit spec note.

3. **`mut.@()`:** This is idiomatic but not explicitly shown in the formal grammar section. It works because `mut` is a modifier and `@()` is an expression. Worth confirming this is tested.

4. **No standalone `@` without `(`:** In expression position, `@` MUST be followed by `(`. The parser enforces this (line 436: `xp(p,TK_LPAREN,"'(' after '@'")`). In type position, `@` can be followed by a type name directly (no paren required for arrays). This asymmetry is intentional but notable.

---

## 9. Conclusion

The `@` sigil is **consistently applied** across the language. It has exactly one semantic domain (collections) with context-dependent parsing that is unambiguous. The design is clean:

- One character, one domain (collections).
- Parser context (type vs. expression) resolves all ambiguity.
- Legacy mode provides `[]` as an alternative for the same semantics.
- No overloading with unrelated concepts.

The only minor inconsistency is the `@(T)` notation in .tki metadata files vs. the `@T` formal grammar for array types. This is a documentation/tooling concern, not a language design issue.
