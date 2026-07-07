---
title: Error Codes
slug: errors
section: reference
order: 3
---

The toke compiler emits structured JSON diagnostics with stable error codes. Error codes are permanent — they are never renumbered or redefined. Each code belongs to a series that identifies the compilation stage that emits it.

## Error Code Series

| Series | Range       | Stage              |
|--------|-------------|--------------------|
| E1xxx  | 1000--1999  | Lexer              |
| E2xxx  | 2000--2999  | Parser             |
| E3xxx  | 3000--3999  | Name Resolution    |
| E4xxx  | 4000--4999  | Type Checker       |
| E5xxx  | 5000--5999  | Arena Validator    |
| E6xxx  | 6000--6999  | IR Lowerer         |
| E9xxx  | 9000--9999  | Codegen / Internal |
| W1xxx  | 1000--1999  | Warnings           |

---

## Lexer Errors (E1xxx)

### E1001

**Invalid escape sequence**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | lex   |

Emitted when a backslash in a string literal is followed by a character that is not one of `"`, `\`, `n`, `t`, `r`, `0`, `x`, or `(`. Also emitted when `\x` is not followed by exactly two hex digits.

<!-- skip-check -->
```text
f=bad(): $str { < "\q" };
```

Triggers E1001.

**Fix:** Use a valid escape sequence. Valid escapes are `\"`, `\\`, `\n`, `\t`, `\r`, `\0`, and `\xHH` (two hex digits).

---

### E1002

**Unterminated string literal**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | lex   |

The lexer reaches end-of-input before encountering a closing `"`.

<!-- skip-check -->
```text
f=bad(): $str { < "unterminated };
```

Triggers E1002.

**Fix:** Add the closing `"` to terminate the string literal.

---

### E1003

**Character outside permitted character set**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | lex   |

Any byte that is not whitespace, alphanumeric, or a recognized symbol triggers this error. toke uses ASCII only.

```text
m=test;
f=bad(): i64 { < 1 };
```

The above is valid; E1003 triggers when non-ASCII characters like `£` appear in source.

**Fix:** Remove or replace the non-ASCII character with valid ASCII.

---

### E1004

**Digit-leading or unterminated identifier**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | lex |

Emitted when an identifier begins with a digit, or a token cannot be closed.

### E1005

**Invalid numeric literal**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | lex |

Emitted for a malformed number (e.g. a bad hex/float form).

### E1006

**Uppercase keyword in default syntax mode**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | lex |

Emitted when an uppercase-cased keyword is used where the default (lowercase) profile expects it lowercase.

## Lexer Warnings (W1xxx)

### W1010

**String interpolation `\(…)` not supported in the legacy profile**

| Field    | Value   |
|----------|---------|
| Severity | warning |
| Stage    | lex     |

Emitted when a `\(` interpolation appears in a string literal while compiling in the
**legacy (80-column) profile**, which does not support interpolation. In the default
profile, `\(expr)` interpolation **is** supported for scalar values (interpolating a
composite value is the compile error `E4032` — see [types](types.md)).

**Fix:** in legacy-profile code, build the string with `str.concat()`; or use the
default profile, where `\(expr)` works.

---

### W1001

**Lossy cast**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | lex |

Warns that a cast loses information (e.g. `f64` to `i64`).

### W1011

**Identifier starts with a keyword prefix**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | lex |

Hint that an identifier begins with a reserved keyword prefix.

### W1020

**Foreign keyword detected**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | lex |

Cross-language hint: a keyword from another language was detected.

## Parser Errors (E2xxx)

### E2001

**Declaration ordering violation**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | parse |

A toke source file must follow the declaration order: `m` (module), `i` (import), `t` (type), `c` (constant), `f` (function). This error fires when a declaration appears out of order or when the `m=` declaration is missing.

<!-- skip-check -->
```text
f=bad(): i64 { < 0 };
```

Triggers E2001 (missing module declaration).

**Fix:** Ensure declarations appear in the required order, starting with `m=`.

---

### E2002

**Unexpected token**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | parse |

The parser encountered a token it cannot consume in the current grammatical context.

```text
m=test;
f=bad(): i64 { < 1 };
```

E2002 triggers when an unexpected token appears (e.g. a stray symbol).

**Fix:** Check for typos or misplaced operators near the reported position.

---

### E2003

**Missing semicolon**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | parse |

A semicolon is required between consecutive statements inside a block. Semicolons may be elided before `}` or at end-of-file.
<!-- skip-check -->
```text
m=test;
f=bad(): i64 { let x = 1 let y = 2; < x };
```

Triggers E2003.

**Fix:** Add a `;` between the two statements.

---

### E2004

**Unclosed delimiter**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | parse |

A `(`, `[`, or `{` was opened but the matching closing delimiter was not found.
<!-- skip-check -->
```text
m=test;
f=bad(): i64 { < (1 + 2 };
```

Triggers E2004.

**Fix:** Add the matching `)`, `]`, or `}`.

---

### E2010

**Pointer type outside extern function**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

Pointer types (`*T`) are only valid in extern (bodyless) function signatures. Using `*T` in a function with a body produces this error.
<!-- skip-check -->
```text
m=test;
f=bad(s: *u8): i64 { < 42 };
```

Triggers E2010.

**Fix:** Either remove the pointer type or make the function an extern declaration (remove the body).

---

### E2005

**Unexpected token in type position**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | parse |

Emitted when a non-type token appears where a type expression is required.

### E2015

**Duplicate field name in struct declaration**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | parse |

Emitted when a struct declares the same field name twice.

## Import/Module Errors (E2xxx continued)

### E2030

**Unresolved import**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | name_resolution |

The module path in an `i=` declaration does not resolve to any `.tki` interface file on the search path.

**Fix:** Check the module path spelling and ensure the dependency is available.

---

### E2031

**Circular import detected**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | name_resolution |

The import graph contains a cycle. Module A imports B which (directly or transitively) imports A.

**Fix:** Restructure modules to break the circular dependency.

---

### E2035

**Malformed version string in import**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | name_resolution |

The optional version string in an import declaration does not match the expected `MAJOR.MINOR` or `MAJOR.MINOR.PATCH` format.
<!-- skip-check -->
```text
m=test;
i=io:std.io "abc";
```

Triggers E2035.

**Fix:** Use a valid version string like `"1.0"` or `"1.0.0"`.

---

### E2036

**No compatible version found**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | name_resolution |

Reserved. Interface files were found but none match the requested version constraint.

---

### E2037

**Version conflict between imports**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | name_resolution |

Two import declarations reference the same module path with different major versions. toke does not support diamond-dependency version conflicts within a single compilation unit.

**Fix:** Align all imports of the same module to the same major version.

---

## Name Resolution Errors (E3xxx)

### E3011

**Identifier not declared**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | name_resolution |

A reference to an identifier that does not exist in any enclosing scope.
<!-- skip-check -->
```text
m=test;
f=bad(): i64 { < x };
```

Triggers E3011.

**Fix:** Declare the identifier before using it, or check for typos.

---

### E3012

**Identifier already declared in this scope**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | name_resolution |

A second declaration of the same name in the same scope. Shadowing across scope boundaries is allowed; duplicate declaration within one scope is not.
<!-- skip-check -->
```text
m=test;
f=bad(): i64 { let x = 1; let x = 2; < x };
```

Triggers E3012.

**Fix:** Use a different name for the second binding, or remove the duplicate.

---

### E3020

**`!` applied to non-error-union value**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

The propagation operator `!` can only be applied to a value whose type is an error union (`T!Err`), and only inside a function whose return type is also an error union.

**Fix:** Ensure the expression has an error union type and the enclosing function returns an error union.

---

## Type Checker Errors (E4xxx)

### E4010

**Non-exhaustive match**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

A `match` expression over a `bool` scrutinee must cover both `true` and `false`.
<!-- skip-check -->
```text
m=test;
f=bad(): i64 { < mt true {$true:v 1} };
```

Triggers E4010 (non-exhaustive match: missing arm for `false`).

**Fix:** Add the missing match arm(s) to cover all cases.

---

### E4011

**Match arms have inconsistent types**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

All arms of a `match` expression must evaluate to the same type.

**Fix:** Ensure all arms return the same type, using explicit casts if needed.

---

### E4025

**Struct has no field with that name**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

A field access expression (`expr.field`) references a field name that does not exist in the struct type.

**Fix:** Check the struct type declaration for the correct field name.

---

### E4031

**Type mismatch**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

The workhorse type error. Emitted for mismatched operands in binary expressions, mismatched function arguments, mismatched binding annotations, mismatched assignment sides, and mismatched return values.

The diagnostic may include a fix suggestion such as `"cast RHS to i64 using 'as'"`.

**Fix:** Ensure both sides of the operation have matching types. Use `as` for explicit casts.

---

### E4040

**Map key type mismatch**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

Reserved. Planned for map key type checking beyond literal consistency.

---

### E4041

**Map value type mismatch**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

Reserved. Planned for map value type checking beyond literal consistency.

---

### E4042

**Method on non-collection type**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

Reserved. Planned for calling collection methods (e.g., `.push`, `.get`) on non-collection types.

---

### E4043

**Inconsistent types in map literal**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

All entries in a map literal must have the same key type and the same value type. The first entry establishes the expected types.
<!-- skip-check -->
```text
m=test;
f=bad(): i64 { < @(1:10; 2:"x") };
```

Triggers E4043.

**Fix:** Ensure all map keys have the same type and all map values have the same type.

---

### E4060

**FFI type mismatch**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | type_check |

Reserved. Planned for type mismatches at FFI boundaries.

---

### E4026

**Wrong argument count in function call**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | typecheck |

Emitted when a call passes too few or too many arguments.

### E4032

**Cannot interpolate a composite value into a string**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

Emitted when a `\(expr)` interpolates an array, struct, or map. Convert it to a string first (e.g. its fields/elements). Fires at code generation.

### E4070

**Assignment to immutable binding**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | typecheck |

Emitted when code assigns to a `let` binding that was not declared mutable.

## Arena Errors (E5xxx)

### E5001

**Value escapes arena scope**

| Field    | Value       |
|----------|-------------|
| Severity | error       |
| Stage    | arena_check |

Inside an `{arena ...}` block, assigning an arena-allocated value to a variable declared in an outer scope would create a dangling reference when the arena is freed.
<!-- skip-check -->
```text
m=test;
f=bad(): i64 { let x = 0; {arena x = 1}; < x };
```
### E5002

**Unreachable code after return**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | arena |

Emitted for statements that follow a `<` return in the same block.

## Codegen / Internal Errors (E9xxx)

Internal and code-generation-stage diagnostics.

### E9001

**Failed to write interface file**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

The compiler could not write the module's `.tki` interface file.

### E9002

**LLVM IR emission failed**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

Code generation could not emit valid LLVM IR (also emitted when a build-time path contains a shell metacharacter).

### E9003

**clang invocation failed**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

The clang/linker sub-process returned a non-zero exit status.

### E9004

**Unresolved stdlib call — missing import**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

A `module.fn()` call refers to a stdlib module that was not imported.

### E9010

**Compiler limit exceeded**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

A fixed compiler capacity was exceeded (e.g. too many locals, functions, or imports).

### E9020

**Failed to write .tkir file**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

The compiler could not write the `.tkir` artifact.

### E9021

**Failed to read .tkir file**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

The compiler could not read a `.tkir` artifact.

## Semantic Warnings (W2xxx / W5xxx / W8xxx)

Non-fatal diagnostics from later stages.

### W2020

**Identifier starts with a keyword**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | parse |

Suggests `let x = …` when an identifier begins with a keyword.

### W2021

**Cross-language pattern detected**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | parse |

A construct idiomatic to another language was detected.

### W2022

**'mut ' used instead of 'mut.'**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | parse |

`mut ` (space) was auto-corrected to `mut.` (dot).

### W2038

**Module name normalised from wrong capitalisation**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | parse |

A module name was normalised from a non-canonical capitalisation.

### W5001

**Value may escape arena scope**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | typecheck |

Escape analysis warns that a value may outlive its arena.

### W8001

**Extern function declaration — FFI unsafe**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | typecheck |

An `extern` function crosses the FFI boundary and is not memory-safe.
