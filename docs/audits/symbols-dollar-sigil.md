# Audit: The `$` Sigil in Toke

**Story:** 77.1.4
**Date:** 2026-05-01
**Status:** Complete

---

## Summary

The `$` sigil is a core structural symbol in toke's default syntax (56-char profile). It serves as a **prefix marker for type names and variant tags**, distinguishing them from plain identifiers. It is NOT part of the identifier token itself — the lexer emits `TK_DOLLAR` as a separate token, and the parser consumes it before reading the following `TK_IDENT`.

---

## 1. Where `$` Appears in toke Source

### 1.1 Type Annotations (parameters, return types, field types)

```
f=getuser(id:u64):$user!$usererr{ ... }
                  ^^^^^  ^^^^^^^^
```

Used whenever a **user-defined type** appears in a type position. Primitives (`i64`, `u64`, `f64`, `bool`, `str`, `byte`) do NOT use `$`.

### 1.2 Type Declarations (after `t=`)

```
t=$user{id:u64;name:str;email:str};
  ^^^^^
```

The type name itself carries `$` in the declaration.

### 1.3 Struct Literals (construction)

```
let p = $point{x: 1; y: 2};
        ^^^^^^
```

### 1.4 Sum Type Variant Tags (definition)

```
t=$usererr{
  $notfound:u64;
  $badinput:str;
  $dberr:str
};
```

Each variant field name is `$`-prefixed. This is how the compiler distinguishes a sum type from a struct type: all fields `$`-prefixed = sum type; no fields `$`-prefixed = struct; mixing = E2011.

### 1.5 Match Arms (pattern matching on variants)

```
match result |
  $ok:v   handle_success(v);
  $err:e  handle_error(e)
```

### 1.6 Error Propagation Target Variant

```
r = db.one(sql;@(id))!$usererr.$db_err;
                       ^^^^^^^^  ^^^^^^^
```

The `!$variant` suffix and the dot-accessed sub-variant both use `$`.

### 1.7 Return Expressions with Variant Constructors

```
<$ok(value);
<$err($calcerr.$invalidrate(s.rate));
```

### 1.8 Array Types (element type)

```
@$payment        -- array of $payment
@$str            -- array of $str
```

### 1.9 Map Types (key/value types)

```
@($str:$user)    -- map from $str to $user
```

---

## 2. Is `$` Part of the Identifier?

**No.** The lexer emits `TK_DOLLAR` as a standalone single-character token (`src/lexer.c:694-700`). The parser then consumes `TK_DOLLAR` + `TK_IDENT` as a two-token sequence and produces a `NODE_TYPE_IDENT` AST node whose text span covers only the identifier portion (the token after `$`).

Evidence from `src/parser.c:313-315`:
```c
if(peek(p)==TK_DOLLAR){Token *dt=adv(p);Token *nt=cur(p);
    if(!xp(p,TK_IDENT,"type name after '$'")){...}
    return mk(p,NODE_TYPE_IDENT,nt);}
```

The node is created from `nt` (the ident), not from `dt` (the dollar). The `$` is purely syntactic sugar consumed by the parser.

However, the spec's token class table (Section 8.1) lists a conceptual `TYPE_IDENT` class described as "`$`-prefixed type reference" with examples `$user`, `$str`, `$i64`. This is the spec's logical view — the implementation splits it into two physical tokens for simplicity.

---

## 3. Primitive Types vs User-Defined Types

| Category | Sigil? | Examples |
|----------|--------|----------|
| Primitive scalars | NO `$` | `i64`, `u64`, `i32`, `u32`, `f64`, `bool`, `str`, `byte`, `void` |
| User-defined struct/sum | YES `$` | `$user`, `$payment`, `$calcerr` |
| Stdlib-provided types | YES `$` | `$str` (when referenced as a type, see note), `$FileErr`, `$DbErr` |
| Built-in result variants | YES `$` | `$ok`, `$err` |

**Important note on `$str`:** The primitive type `str` (no sigil) is used in type positions like parameter types: `name:str`. However, when `str` appears as a stdlib module type reference in certain contexts (e.g., `$str` in function signatures in the spec's stdlib section, FFI types), the `$` form appears. The spec states at line 1075: "primitive types are written without the `$` sigil: `i64`, `str`, `bool`." In practice, both `str` and `$str` appear in the corpus and stdlib — this is an area where the compiler accepts both forms for string type.

---

## 4. Where `$` Does NOT Appear

- Variable names: `let count = 5;` (never `$count`)
- Function names: `f=getuser(...)` (never `f=$getuser`)
- Module names: `m=calc;` (never `m=$calc`)
- Import names: `i=http;` (never `i=$http`)
- Primitive type annotations in parameters: `id:u64` (never `id:$u64`)
- Field access: `user.name` (never `user.$name`)
- Operators and keywords

---

## 5. Legacy Profile (80-char)

In the legacy profile (`--legacy`), `$` is **not available** (E1003 error if encountered). Instead:
- Type names use PascalCase: `User`, `Str`, `Point`
- Struct literals use PascalCase: `Point{x: 1; y: 2}`
- Arrays use `[...]` syntax instead of `@(...)`

The lexer explicitly rejects `$` in legacy mode (`src/lexer.c:695-698`):
```c
case '$':
    if (l.profile == PROFILE_LEGACY) {
        diag_emit(DIAG_ERROR, LEX_E1003, ...);
        return -1;
    }
    sym = TK_DOLLAR; break;
```

---

## 6. Compiler Enforcement

The compiler **consistently enforces** `$` in the default profile:

| Context | Parser location | Behavior |
|---------|----------------|----------|
| Type expressions | `parse_type_expr()` L312-315 | Expects `TK_DOLLAR` + `TK_IDENT` for user types |
| Struct literals | `parse_primary()` L413-431 | `$name{...}` triggers struct literal parse |
| Match arms | `parse_match_arm()` L722-731 | `$variant:binding expr` |
| Type declarations | Top-level L1414-1417 | `t=$name{...}` |
| Field lists (sum) | `parse_field_list()` L1129-1130 | `$variant:type` in sum type body |

If `$` is missing where a user-defined type is expected, the parser falls through to other production rules and typically emits E2002 ("unexpected token") or E2005 ("unexpected token in type position").

---

## 7. Corpus Usage Patterns

The toke-corpus confirms heavy, consistent `$` usage across all generated programs:
- Type references: `$str`, `$vec2`, `$user`
- Error types: `$dberr`, `$apierr`, `$fileerr`
- Match arms: `$ok:v`, `$err:e`
- Struct construction: `$scenario{...}`, `$payment{...}`
- Array types: `@$str`, `@$payment`

No corpus examples omit `$` for user-defined types.

---

## 8. Could Toke Work Without `$`?

Theoretically yes, but with trade-offs:

**What `$` provides:**
1. **Visual disambiguation** — instantly tells the reader "this is a type, not a variable"
2. **Parser simplicity** — the parser can identify type positions without lookahead or context-sensitive rules
3. **No case sensitivity dependency** — unlike legacy mode (PascalCase), the `$` approach works with all-lowercase identifiers
4. **Sum type clarity** — `$variant:type` in definitions and `$variant:binding` in match arms are unambiguous

**Without `$`, the language would need:**
- PascalCase convention (legacy mode already does this)
- Or contextual parsing (heavier grammar, potential ambiguities)
- Or a different sigil/keyword system

The legacy profile demonstrates that toke CAN work without `$` — it uses uppercase-initial identifiers for types instead. The two profiles are isomorphic: `$user` (default) = `User` (legacy).

---

## 9. Complete Taxonomy of `$`-Prefixed Forms

| Form | Meaning | Example |
|------|---------|---------|
| `$name` in type position | User-defined type reference | `param:$user` |
| `$name{...}` in expression | Struct literal constructor | `$point{x:1;y:2}` |
| `$name` in `t=$name{...}` | Type declaration name | `t=$user{...}` |
| `$variant` in sum type body | Variant tag definition | `$notfound:u64` |
| `$variant:binding` in match | Pattern match on variant | `$ok:v expr` |
| `!$errtype` in signature | Error type annotation | `:$user!$usererr` |
| `!$errtype.$variant` | Error propagation target | `!$usererr.$db_err` |
| `$ok(val)` / `$err(val)` | Result variant construction | `<$ok(user)` |
| `@$type` | Array of user type | `@$payment` |
| `@($k:$v)` | Map with user types | `@($str:$user)` |
| `*$type` | Pointer to user type (FFI) | `*$str` |

---

## 10. Conclusion

The `$` sigil is a deliberate, consistently-enforced design choice that:
- Marks ALL user-defined type names and variant tags
- Does NOT apply to primitive types (`i64`, `str`, `bool`, etc.)
- Is a separate lexer token, not part of the identifier
- Has a direct equivalent in legacy mode (PascalCase)
- Is required by the compiler in all type-referencing contexts in default mode

It is one of toke's most distinctive syntactic features and appears in every non-trivial toke program.
