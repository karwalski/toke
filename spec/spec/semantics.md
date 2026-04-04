# toke Formal Semantics

**Status:** Normative stub (Story 2.12.2)
**Profile:** 1 (v0.1)

This document defines the static semantics of the toke language: the type system,
name resolution rules, and type checking judgements that a conforming implementation
must enforce. Runtime semantics (evaluation order, memory model) are out of scope
for this document; see `spec/memory.md` (planned).

Cross-references use the notation `[errors.md E4031]` for error codes and
`[grammar.ebnf <Production>]` for grammar productions. Where the grammar stub
has not yet been formalized, the production name refers to the corresponding
`NODE_*` kind in the reference compiler.

---

## 1. Type Universe

### 1.1 Primitive Types

A conforming implementation SHALL support the following primitive types:

| Type   | Description                  | Zero value |
|--------|------------------------------|------------|
| `i64`  | Signed 64-bit integer        | `0`        |
| `u64`  | Unsigned 64-bit integer      | `0`        |
| `f64`  | IEEE 754 double-precision    | `0.0`      |
| `bool` | Boolean (`true` or `false`)  | `false`    |
| `Str`  | Immutable UTF-8 string       | `""`       |
| `void` | Unit type (no value)         | n/a        |

> **Note:** `void` is an implementation-defined type used by the compiler for
> functions with no return value and for statement-form expressions. It is not
> listed in the main spec's primitive type table (Section 13.1) and should be
> considered an internal compiler type rather than a user-facing primitive.

The type keywords `i64`, `u64`, `f64`, `bool`, `Str`, and `void` are predefined
identifiers (Section 4.3) and are always in scope.

### 1.1a Additional Primitive Types (spec-defined, not yet implemented)

The main spec (Section 13.1) defines additional primitive types that are not yet
implemented in the reference compiler:

| Type   | Description                  | Status              |
|--------|------------------------------|---------------------|
| `Byte` | Single byte (u8 alias)       | Spec-defined        |
| `i8`   | Signed 8-bit integer         | Not yet implemented |
| `i16`  | Signed 16-bit integer        | Not yet implemented |
| `i32`  | Signed 32-bit integer        | Not yet implemented |
| `u8`   | Unsigned 8-bit integer       | Not yet implemented |
| `u16`  | Unsigned 16-bit integer      | Not yet implemented |
| `u32`  | Unsigned 32-bit integer      | Not yet implemented |
| `f32`  | IEEE 754 single-precision    | Not yet implemented |

A conforming Profile 1 implementation need only support the types in Section 1.1.
The full type table will be required in a future profile.

### 1.2 Composite Types

| Type syntax | Kind        | Description                                     |
|-------------|-------------|-------------------------------------------------|
| `[T]`       | Array       | Ordered, homogeneous sequence of element type T  |
| `[K:V]`     | Map         | Key-value collection; K and V are homogeneous    |
| `T!Err`     | Error union | Sum type: either a value of type T or an error   |
| `*T`        | Pointer     | Raw pointer to T (FFI only, Section 7.2)         |

**Array.** The element type is determined by the first element of an array
literal, or by an explicit type annotation on the binding. An empty array
literal `[]` has element type `unknown` until constrained by context.

**Map.** The key and value types are determined by the first entry of a map
literal. All subsequent entries must have key and value types equal to the
first entry's types. Violation: `[errors.md E4043]`.

**Error union.** Written `T!Err` where `T` is the success type and `Err` is
the error type. The `!` propagation operator (Section 5) unwraps the success
value or propagates the error to the enclosing function's return.

**Pointer.** The `*T` type is restricted to extern (bodyless) function
signatures. Use of `*T` in a function with a body is a compile error
`[errors.md E2010]`.

### 1.3 Named Types

| Type syntax | Kind     | Description                               |
|-------------|----------|-------------------------------------------|
| `T=Name{}`  | Struct   | Named product type with ordered fields     |
| `Task<T>`   | Task     | Async task handle yielding type T          |
| `func`      | Function | First-class function reference (internal)  |

**Struct.** Declared via `T=Name{field1:Type1; field2:Type2; ...}`.
Two struct types are equal if and only if they have the same name (nominal
typing). Field access is via dot notation: `expr.field`. Accessing a
non-existent field is a compile error `[errors.md E4025]`.

**Task.** The type `Task<T>` is the return type of `spawn(f)` where `f`
returns type `T`. `Task` is not directly writable in source; it is
inferred from `spawn` expressions. `await(t)` on a `Task<T>` yields type `T`.

### 1.4 The Unknown Type

The internal type `unknown` is a sentinel used when a sub-expression has
already emitted an error diagnostic. A value of type `unknown` is compatible
with any other type for the purpose of further type checking (to suppress
cascading errors). `unknown` SHALL NOT appear in well-typed programs.

### 1.5 Numeric Type Classification

A type is *numeric* if and only if it is one of: `i64`, `u64`, `f64`.

---

## 2. Type Inference Rules

This section defines how the type of each expression form is determined.

### 2.1 Literals

| Expression form           | Inferred type | Notes                          |
|---------------------------|---------------|--------------------------------|
| Integer literal           | `i64`         | `[grammar.ebnf IntLit]`        |
| Floating-point literal    | `f64`         | `[grammar.ebnf FloatLit]`      |
| String literal            | `Str`         | `[grammar.ebnf StrLit]`        |
| Boolean literal           | `bool`        | `true` or `false`              |
| Array literal `[e1;e2;..]`| `[T]`         | T = type of first element      |
| Empty array literal `[]`  | `[unknown]`   | Constrained by annotation      |
| Map literal `[k1:v1;..]`  | `[K:V]`       | K,V from first entry           |
| Struct literal `Name{..}` | `Name`        | Resolved via type declaration  |

### 2.2 Identifier Expressions

The type of an identifier is determined by its declaration:

1. If the identifier refers to a `let` binding (immutable or `let x=mut.expr`
   mutable) with a type annotation, the type is the annotated type.
2. If the identifier refers to a `let` binding without a type annotation,
   the type is inferred from the initializer expression.
3. If the identifier refers to a function parameter, the type is the
   parameter's declared type.
4. If the identifier refers to a function declaration, the type is `func`.
5. Otherwise the type is `unknown`.

### 2.3 Unary Expressions

For a unary expression `op expr`:

- **Negation (`-`):** The operand must be numeric. Specifically, it must have
  type `i64` or `f64`. Applying `-` to `u64` or any non-numeric type is a
  compile error `[errors.md E4031]`. The result type is the operand type.
- **Logical not (`!` prefix):** STUB -- not yet formalized.

### 2.4 Binary Expressions

For a binary expression `lhs op rhs`:

**Arithmetic operators** (`+`, `-`, `*`, `/`):
- Both operands must be numeric.
- Both operands must have equal types (no implicit widening).
- The result type is the common operand type.
- Violation: `[errors.md E4031]` with fix suggestion `"cast RHS to <T> using 'as'"`.

**Comparison operators** (`<`, `>`, `=`):
- Both operands must have equal types.
- The result type is `bool`.
- Violation: `[errors.md E4031]`.

**Logical operators** (`&&`, `||`): STUB -- not yet formalized in the type checker.

### 2.5 Cast Expressions

A cast expression `expr as T` evaluates `expr` and produces a value of type `T`.
The source expression type is inferred but not constrained against the target
type -- all casts are unconditionally allowed in Profile 1.

STUB: A future profile may restrict which type pairs are castable.

### 2.6 Call Expressions

For a call expression `f(a1, a2, ...)`:

1. The callee `f` is looked up in the scope chain.
2. If `f` is a declared function, each argument type is checked against the
   corresponding parameter type. Mismatch: `[errors.md E4031]`.
3. The result type is the function's declared return type, or `void` if no
   return type is specified.
4. Extra arguments beyond the declared parameters are inferred but not checked
   against any parameter type (they are silently accepted in Profile 1).

**Special call forms** are described in Section 7.1 (`spawn`, `await`).

### 2.7 Field Access Expressions

For a field access `expr.field`:

- If `expr` has type `[T]` (array) or `[K:V]` (map) and `field` is `len`,
  the result type is `u64`.
- If `expr` has a struct type, the field name is looked up in the struct's
  field list. If found, the result type is the field's declared type.
  If not found: `[errors.md E4025]`.
- Otherwise, the result type is `unknown`.

### 2.8 Binding Statements

For `let x = expr;` (immutable) or `let x=mut.expr;` (mutable):

- If both a type annotation `T` and an initializer `expr` are present, the
  annotation type and the initializer type must be equal. Violation:
  `[errors.md E4031]`.
- The binding itself has type `void` (it is a statement, not an expression).
- The `mut.` qualifier marks the binding as mutable; see `[grammar.ebnf MutBindStmt]`.

### 2.9 Assignment Statements

For `x = expr`:

- The left-hand side type and right-hand side type must be equal.
  Violation: `[errors.md E4031]`.
- The binding must have been declared with `let x=mut.` (mutable binding).
  STUB: mutability checking is not yet formalized in the type checker.

### 2.10 Return Statements

For `< expr` (return):

- The expression type must equal the enclosing function's declared return type.
  Violation: `[errors.md E4031]`.
- If the function has no return type annotation, the expected return type is `void`.

---

## 3. Type Compatibility

### 3.1 Type Equality

Two types are *equal* according to the following rules:

1. **Identity:** A type is equal to itself.
2. **Primitives:** Two primitive types are equal iff they have the same kind
   (e.g., `i64 == i64`, `i64 != u64`).
3. **Structs:** Two struct types are equal iff they have the same name
   (nominal equality).
4. **Pointers:** `*T == *U` iff `T == U`.
5. **Arrays:** `[T] == [U]` iff `T == U`, or if either element type is `unknown`.
6. **Maps:** `[K1:V1] == [K2:V2]` iff `K1 == K2` and `V1 == V2`, or if either
   key or value type is `unknown`.
7. **Tasks:** `Task<T> == Task<U>` iff `T == U`.
8. **Error unions:** `T1!E1 == T2!E2` iff `T1 == T2`, or if either inner type
   is `unknown`.
9. **Functions:** Two function types are equal iff their return types are equal.
   STUB: parameter type lists are not yet compared.

### 3.2 Implicit Coercions

Profile 1 defines **no implicit coercions**. There is no automatic widening
(e.g., `i64` to `f64`), no automatic narrowing, and no implicit conversion
between numeric types.

All type conversions must use an explicit `as` cast (Section 2.5).

### 3.3 The Unknown Compatibility Rule

A type `unknown` is compatible with any other type. This is not a coercion;
it is an error-recovery mechanism. When the type checker encounters `unknown`,
it suppresses further type mismatch diagnostics to avoid cascading errors
from a single root cause.

---

## 4. Name Resolution

### 4.1 Scope Chain

Name resolution uses a chain of lexical scopes. The scope chain, from outermost
to innermost, is:

1. **Module scope** -- contains predefined identifiers (Section 4.3), import
   aliases (Section 6), and all top-level declarations (`F=`, `T=`, constants).
2. **Function scope** -- contains the function's parameters. Created when
   entering a `F=` declaration body.
3. **Block scope** -- created for each `{ ... }` statement list, `lp()` loop,
   and match arm (`|{...}`). Nested blocks create nested scopes.

Lookup proceeds from the innermost scope outward. The first matching declaration
is returned.

### 4.2 Declaration Kinds

| Kind              | Introduced by           | Mutable | Notes                        |
|-------------------|-------------------------|---------|------------------------------|
| `DECL_FUNC`       | `F=name(...)`           | no      | Forward-declared in pass 1   |
| `DECL_TYPE`       | `T=Name{...}`           | no      | Forward-declared in pass 1   |
| `DECL_CONST`      | `name = literal : Type;`| no      | Forward-declared in pass 1   |
| `DECL_PARAM`      | Function parameter      | no      | Bound in function scope      |
| `DECL_LET`        | `let x = ...`           | no      | Bound in enclosing block     |
| `DECL_MUT`        | `let x=mut.expr`        | yes     | Bound in enclosing block     |
| `DECL_PREDEFINED` | Built-in                | no      | Always in module scope       |
| `DECL_IMPORT_ALIAS` | `I=alias:path`        | no      | Resolved import alias        |

> **Constant syntax discrepancy:** The spec grammar `[grammar.ebnf ConstDecl]`
> defines constant declarations as `IDENT = LiteralExpr : TypeExpr ;` (e.g.,
> `PI = 3.14159 : f64;`). The reference compiler currently uses a `C=` sigil
> prefix (`C=PI 3.14159:f64;`). This document follows the spec grammar form;
> compiler output may differ until the compiler is updated.

### 4.3 Predefined Identifiers

The following identifiers are seeded into the module scope before any
user-declared names:

    true   false   bool   i64   u64   f64   Str   void   spawn   await   Task

These cannot be shadowed by module-level declarations (since they occupy the
same scope), but they CAN be shadowed by function parameters or local bindings
in inner scopes.

### 4.4 Forward Declaration

All module-scope declarations (`F=`, `T=`, constants) are registered in a first
pass before any references are resolved. This means:
- Functions may call other functions declared later in the file.
- Types may reference other types declared later in the file.
- A function may reference itself (recursion).

### 4.5 Shadowing Rules

- **Same-scope duplicate:** Declaring a name that already exists in the
  *same* scope is a compile error `[errors.md E3012]`.
- **Cross-scope shadowing:** Declaring a name in an inner scope that shadows
  an outer-scope name is permitted. The inner declaration takes precedence
  within its scope.

### 4.6 Reference Resolution

When an identifier is referenced:

1. The scope chain is searched from innermost to outermost.
2. If found, the reference is bound to that declaration.
3. If not found in any scope: `[errors.md E3011]`.

**Field names** (the right-hand side of `expr.field`) are NOT resolved by
name resolution. They are resolved by the type checker using the struct's
field list.

### 4.7 Loop Scoping

A `lp(init; cond; step) { body }` loop creates a single scope that encloses
the init variable, condition, step, and body. The init variable (if declared
with `let` or `let x=mut.`) is visible in all three subsequent positions.

### 4.8 Match Arm Scoping

Each match arm creates its own scope. A binding introduced in a match arm
pattern is visible only within that arm's body.

---

## 5. Error Type Propagation

### 5.1 Error Union Types

An error union type `T!Err` represents a value that is either a success
value of type `T` or an error value. Error unions are declared in function
return types.

### 5.2 The Propagation Operator `!`

The postfix `!` operator (propagate) may be applied to an expression of
error union type. Its semantics are:

1. The expression must have type `T!Err`. If not: `[errors.md E3020]`.
2. The enclosing function must have a return type that is also an error union.
   If not: `[errors.md E3020]`.
3. If the value is the error variant, the error is returned from the
   enclosing function immediately (early return).
4. If the value is the success variant, the `!` operator unwraps it and
   produces a value of type `T`.

### 5.3 Match on Error Types

STUB: Matching on error union variants (e.g., `result|{ Ok:v expr; Err:e expr }`)
is planned but not yet formalized. The current type checker validates match
exhaustiveness only for `bool` scrutinees.

---

## 6. Module Visibility

### 6.1 Import Declarations

An import declaration `I=alias:module.path` or `I=alias:module.path "version"`
introduces an import alias into the module scope.

**Resolution order:**
1. If the path starts with `std.`, the import is always resolved (standard
   library stub).
2. Otherwise, the compiler searches `<search_path>/<module/path>.tki` for
   a matching interface file.
3. If a version string is present, the compiler first tries the versioned
   path `<module/path>.<version>.tki`.

### 6.2 Interface Files (.tokei / .tki)

Interface files (`.tki`) export the public signatures of a module. They
contain function signatures, type declarations, and constant declarations
without implementation bodies.

STUB: The precise format of `.tki` files is not yet formalized in this
specification. The reference compiler emits `.tki` files during codegen.

### 6.3 Version Constraints

Import version strings must match the format `MAJOR.MINOR` or
`MAJOR.MINOR.PATCH`. Malformed version strings: `[errors.md E2035]`.

Two imports of the same module with different major versions are a compile
error `[errors.md E2037]`. This prevents diamond-dependency version conflicts
within a single compilation unit.

### 6.4 Circular Import Detection

If module A imports module B which (directly or transitively) imports module A,
the compiler emits `[errors.md E2031]`. Circular imports are prohibited.

### 6.5 Declaration Ordering

Within a source file, declarations must appear in the following order:

1. Module declaration (`M=`)
2. Import declarations (`I=`)
3. Type declarations (`T=`)
4. Constant declarations (`C=`)
5. Function declarations (`F=`)

Violation: `[errors.md E2001]`.

---

## 7. Special Forms

### 7.1 Concurrency: spawn and await

**spawn(f):**
- `f` must be a declared function (not an arbitrary expression).
  Violation: `[errors.md E4050]`.
- In Profile 1, `f` must be nullary (zero parameters).
  Violation: `[errors.md E4052]`.
- The result type is `Task<T>` where `T` is the return type of `f`.
  If `f` has no return type annotation, `T` is `void`.

**await(t):**
- `t` must have type `Task<T>`. Violation: `[errors.md E4051]`.
- The result type is `T` (the inner type of the task).

`spawn` and `await` are predefined identifiers, not keywords. They can
theoretically be shadowed (Section 4.5), though doing so is not recommended.

> **Note:** `spawn`, `await`, and `Task` are Phase 2 additions to the language.
> They do not yet appear in the main spec's reserved identifier list (Section 14).
> Their semantics are defined here based on the reference compiler implementation
> and are subject to change when formally incorporated into the main spec.

### 7.2 FFI: Extern Functions

An extern function is a function declaration without a body:
```
F=name(param1:Type1; param2:Type2):ReturnType;
```

Extern functions:
- Are the ONLY context where pointer types `*T` are permitted.
  Use of `*T` in a function with a body: `[errors.md E2010]`.
- Are assumed to be implemented externally (linked at compile time via clang).
- Have their parameter and return types checked at call sites like any other
  function.

The `contains_ptr` check is recursive: `[*T]` (array of pointers) or
`*[T]` (pointer to array) are both detected and rejected in non-extern
context.

### 7.3 Arena Blocks

An `{arena ...}` block introduces a region-scoped allocation context.

**Escape analysis:** Assigning an arena-allocated value to a variable
declared in an outer scope is a compile error `[errors.md E5001]`. This
prevents dangling references when the arena is freed at block exit.

The arena depth is tracked during type checking. STUB: Full formalization
of arena lifetime rules requires the memory model specification.

---

## 8. Match Expressions

### 8.1 Typing

A match expression has a scrutinee expression and one or more arms, written
in spec-canonical postfix-pipe form `[grammar.ebnf MatchExpr]`:
```
scrutinee|{
    Variant1:binding1 body1;
    Variant2:binding2 body2
}
```

Note: `match` is NOT a keyword. The match operator is the postfix `|{...}` form.

**Type rules:**
1. The scrutinee is inferred to some type `S`.
2. Each arm body is inferred to some type `Ti`.
3. All arm body types must be equal. Violation: `[errors.md E4011]`.
4. The result type of the match expression is the common arm type.

### 8.2 Exhaustiveness

**Bool scrutinee:** Both `true` and `false` arms must be present.
Missing either arm: `[errors.md E4010]`.

STUB: Exhaustiveness checking for struct types, error union types, and
integer ranges is not yet implemented. A future profile will formalize
pattern coverage for all scrutinee types.

---

## 9. Built-in Properties

### 9.1 Collection Length

The `.len` property is available on arrays and maps:

- `expr.len` where `expr : [T]` has type `u64`.
- `expr.len` where `expr : [K:V]` has type `u64`.

Accessing `.len` on a non-collection type falls through to struct field
lookup (and likely produces `[errors.md E4025]` or type `unknown`).

---

## 10. Map Literal Consistency

A map literal `[k1:v1; k2:v2; ...]` is typed as follows:

1. The key type `K` is the inferred type of the first key `k1`.
2. The value type `V` is the inferred type of the first value `v1`.
3. For each subsequent entry `ki:vi`:
   - If `type(ki) != K` and neither is `unknown`: `[errors.md E4043]`.
   - If `type(vi) != V` and neither is `unknown`: `[errors.md E4043]`.
4. The map literal type is `[K:V]`.

---

## Appendix A. Error Code Cross-Reference

The following error codes are referenced by this specification:

| Code  | Section(s) | Short description                          |
|-------|------------|--------------------------------------------|
| E2001 | 6.5        | Declaration ordering violation              |
| E2010 | 1.2, 7.2   | Pointer type outside extern function        |
| E2030 | 6.1        | Unresolved import                           |
| E2031 | 6.4        | Circular import detected                    |
| E2035 | 6.3        | Malformed version string                    |
| E2037 | 6.3        | Version conflict between imports            |
| E3011 | 4.6        | Identifier not declared                     |
| E3012 | 4.5        | Identifier already declared in scope        |
| E3020 | 5.2        | `!` on non-error-union value                |
| E4010 | 8.2        | Non-exhaustive match                        |
| E4011 | 8.1        | Match arms have inconsistent types          |
| E4025 | 2.7, 1.3   | Struct has no field with that name          |
| E4031 | 2.3-2.10   | Type mismatch                               |
| E4043 | 1.2, 10    | Inconsistent types in map literal           |
| E4050 | 7.1        | spawn argument not callable                 |
| E4051 | 7.1        | await argument not a Task                   |
| E4052 | 7.1        | Spawned function has parameters             |
| E5001 | 7.3        | Value escapes arena scope                   |

---

## Appendix B. STUB Inventory

The following areas are marked STUB and require further formalization:

| Section | Topic                                          | Blocking on           |
|---------|------------------------------------------------|-----------------------|
| 2.3     | Logical not operator typing                    | Grammar formalization |
| 2.4     | Logical operator (`&&`, `||`) typing           | Grammar formalization |
| 2.5     | Cast restriction rules (which pairs allowed)   | Profile 2 design     |
| 2.9     | Mutability enforcement on assignment LHS       | Type checker update   |
| 3.1.9   | Function type equality (parameter lists)       | Type checker update   |
| 5.3     | Match on error union variants                  | Error type design     |
| 6.2     | Interface file format specification             | Spec amendment        |
| 7.3     | Arena lifetime formal rules                    | Memory model spec     |
| 8.2     | Exhaustiveness for non-bool scrutinees         | Profile 2 design     |
