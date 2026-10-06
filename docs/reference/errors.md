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
| E8xxx  | 8000--8999  | FFI                |
| E9xxx  | 9000--9999  | Codegen / Internal |
| Wnxxx  | same ranges | Warnings — the number, not the emitting pass, sets the stage |

The `stage` field of every JSON diagnostic is derived from the code's number
(`phase_for_code` in `src/diag.c`), so it reads `lex`, `parse`,
`name_resolution`, `type_check`, `arena_check`, `ir_lower`, `ffi` or `codegen`.
Each section's **Stage** row gives that value first; when another pass
actually emits the code, the row says so in parentheses (for example E2030 is
`parse`, emitted by name resolution). `make check-error-codes` gates both rows.

The `source_line` field holds the text of the diagnostic's line. A line longer
than 1023 bytes is cut on a UTF-8 character boundary, never inside a
multi-byte character, and the record then also carries
`"source_line_truncated": true`. The field is absent when the line is whole.

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

### E1007

**Unterminated comment**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | lex |

Emitted when a `(*` comment is still open at end of file. Comments nest, so a
`(*` inside a comment (e.g. `COUNT(*)`) opens another level and takes the `*)`
meant for the outer one. Reported at the outermost `(*`, naming the nested
opener. No fix field (story 138.10).

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
composite value is the compile error `E4032` — see [types](/docs/reference/types/)).

**Fix:** in legacy-profile code, build the string with `str.concat()`; or use the
default profile, where `\(expr)` works.

---

### W1001

**Lossy cast**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | type_check |

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

The same code covers foreign comment markers. toke's comment form is
`(* ... *)` (spec v0.4 §H, story 127.198); `//` and `#` are not comment syntax,
so the lexer skips each to end of line as recovery and warns "toke comments are
written (* comment *)". **Fix:** `replace '// comment' with '(* comment *)'`
(or `'# comment'`), offered only when the comment's text holds no `(*` or `*)`
-- either one would change where the rewritten comment ends. Pinned by D201
and D202.

Not emitted where toke has a name spelled like the keyword (story 138.10):
after `let`, before `=` or a `:` that does not end the line, and -- for the
statement and declaration keywords (`def`, `return`, `class`, `var`, `else`,
`while`, ...) -- before an operator or closing punctuation (`def.a`,
`f(def)`). Value-like words (`None`, `null`, `nil`, `undefined`, `True`,
`False`) and `print` / `printf` / `console` still warn in value position.

### W1021

**Raw control byte in string literal**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | lex |

A string literal's text holds a raw control byte: 0x00-0x08, 0x0B, 0x0C,
0x0E-0x1A or 0x1C-0x1F. Tab, line feed, carriage return and ESC (0x1B) are
not reported. The byte compiles to the same string as its escape, but it is
invisible in an editor and a raw NUL cuts `source_line` short; the corpus
control-byte guard rejects a program that holds one (story 131.32).

**Fix:** replace the byte with its `\xHH` escape. The `fix` field names it
(`replace the raw byte with \x00`); the escape and the byte are the same
string, so the fix is exact. One warning per byte. A control byte inside a
`\(...)` interpolation is outside the literal's text and is E1003.

It is a warning, not an error, because the spec permits any UTF-8 in string
literal content (`toke-spec-v0.4.md` §G).

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
| Stage    | parse (emitted by the type checker) |

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
| Stage    | parse (emitted by name resolution) |

The module path in an `i=` declaration does not resolve to any `.tki` interface file on the search path.

A `std.*` path is checked the same way (story 127.42): a standard-library module is real only when it is registered in the compiler's native-glue registry or ships a `.tki` interface. Before that check, any `std.<anything>` was accepted and the generic `tk_<mod>_<m>_w` call rule fabricated symbols for it, so a typo surfaced only as a link failure — or, under `--check`, not at all.

**Fix:** Check the module path spelling and ensure the dependency is available. When the name is within edit distance 2 of exactly one known module the diagnostic carries a `did you mean 'std.<module>'?` fix; otherwise no fix is offered, because the intended module is not determined.

---

### E2031

**Circular import detected**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | parse (emitted by name resolution) |

The import graph contains a cycle. Module A imports B which (directly or transitively) imports A.

**Fix:** Restructure modules to break the circular dependency.

---

### E2035

**Malformed version string in import**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | parse (emitted by name resolution) |

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
| Stage    | parse (reserved for name resolution; not yet emitted) |

Reserved. Interface files were found but none match the requested version constraint.

---

### E2037

**Version conflict between imports**

| Field    | Value           |
|----------|-----------------|
| Severity | error           |
| Stage    | parse (emitted by name resolution) |

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

A second declaration of the same name in the same scope, for a kind that may
not be redeclared: `f=` functions, `t=` types, `i=` imports, consts and
parameters.

`let` and `let mut` are **excluded**: story 75.1.7 chose "allow same-scope
and cross-scope shadowing", and §11.8.1 of the spec publishes
`let x = 1; let x = x + 1;` as a valid program that yields 2. A re-binding
`let` never triggers E3012; a cross-scope one raises the W3013 warning.
<!-- skip-check -->
```text
m=test;
t=box{n: i64};
t=box{m: i64};
```

Triggers E3012 on the second `t=box`.

**Fix:** Use a different name for the second declaration, or remove the duplicate.

---

### E3020

**`!` applied to non-error-union value**

| Field    | Value      |
|----------|------------|
| Severity | error      |
| Stage    | name_resolution (emitted by the type checker) |

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

A result match whose value is used must have both an `$ok` arm and an
error arm (`$err`, or the variants of the error sum); without one, that side
would yield the zero value of the result type.  A result match standing as a
statement may have just one: `mt g() {$err:e io.println("failed")};` runs only
on failure.
<!-- skip-check -->
```text
let a=mt g() {$ok:v v};
```

Triggers E4010 (non-exhaustive match: missing arm for `$err`), with no `fix`.

The last statement of a branch of an expression-`if` is that `if`'s value, so
a match there is used too (story 127.252): `let a=if(c){ mt g() {$ok:v v} }el{ 5 };`
is E4010. An `if` standing as a statement discards its branches' values.

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

**Composite types print structurally** (127.54). `expected` and `got` render an
array as `@(elem)`, a map as `@(key:value)` and a function as `func->ret`, so
two types the checker treats as different always read as different. They used
to print as the bare kind, which produced diagnostics like
`type mismatch: expected 'array', got 'array'` — a mismatch reported between a
type and itself, which an automated repair loop cannot act on. An element type
the compiler has not established prints as `?`, as in `@(?)`.

**`str + str` is not reported as a mismatch between `str` and `str`** (127.140).
`+` is numeric-only in toke (ADR-0004), so the mismatch is between what the
operator requires and what both operands are: `expected` is `a numeric type`
and `got` is `str`. Its `fix` names the four canonical string-building
patterns, and the argument order in each has been executed rather than read —
`str.join` takes the **separator as its receiver**, so the call is
`s.join(sep;arr)`. Until 127.140 this field recommended `s.join(arr;sep)`,
which passes `--check`, builds, and exits 139.

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

A body-less extern (`f=malloc(n:i64):i64;`) names a C function that the
compiler itself declares and calls -- `malloc` (array literals, closure
environments), and `fprintf` and `exit` (the `--max-iters` loop guard) --
with a different signature.  A symbol has one declaration, so the compiler's
own calls and the extern cannot both be right.  Emitted during code
generation (story 127.219), so `--check` does not report it.  An extern that
repeats the compiler's own declaration exactly is accepted.  No `fix`:
changing the signature or binding a different symbol is the author's choice.
Conformance: `test/conform/C211_extern_own_c_symbol.sh`.

---

### E4026

**Wrong argument count in function call**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

Emitted when a call passes too few or too many arguments.

Since 136.1 this also covers a call through an import alias, `alias.member(...)`. Such a call is judged against the *implementation* where the compiler knows the glue symbol — that is what decides whether the call corrupts — and against the `.tki` interface otherwise. The message names the function and, for the interface case, the declared signature, because a bare count does not say which side is wrong: an interface and its implementation can be the two things that disagree.

Since 127.1 it also covers a member call on a VALUE receiver, `recv.member(args)`. The receiver counts as the method's first argument, and the arity is read from the compiler's record of the native side — but only when every glue symbol that member may lower to agrees on it, since the symbol chosen depends on the receiver's kind (`contains` is a hash lookup on a map, a linear scan on an array and a substring search on a string). Where they disagree the compiler has no established arity and says nothing. Before this, surplus arguments were **discarded in silence** on the method path: `a.concat("A";"B")` on `a="xx"` compiled clean and returned `"xxA"`, and `a.len(1;2;3)` returned the length.

**Fix:** Match the call to the declaration, or reconcile the interface with the implementation if they disagree. For a receiver method, the receiver is the first argument.

### E4027

**Module has no exported member with that name**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

Emitted for `alias.member(...)` when neither the imported module's `.tki` interface nor the runtime declares `member`. Before 136.1 such a call was lowered to a symbol name anyway and surfaced as an E9003 link failure naming a mangled symbol, with no file and no line.

The check is deliberately narrow. A `std.*` member that the hand-written interface omits but the runtime does provide (`str.equals`, `test.eq`, `json.getobj`, …) is accepted: the call works and the interface is the incomplete side. A member of a module whose `.tki` is machine-generated is enforced in full, because `--emit-interface` writes one record per top-level `f=` and that export list is complete by construction.

E4036 is the receiver-call form of the same question: `recv.member(...)` where `recv` is a value rather than an import alias.

**An alias that collides with a type the module exports is still checked**
(136.63). The check skips a call whose base names a module-scope function,
const or binding, because then the identifier is not the module. A *type* of
that name is not a skip reason: toke has no value of a type's name (see
E4033), so `alias.member(...)` in call position must be the import. Treating a
type as a shadow silently disabled this whole check for any module exporting a
type named like its own alias — the most natural alias a user would pick.
`i=canvas:std.canvas;` plus `canvas.nosuchmemberatall(c)` passed `--check` and
died at the linker, while the identical program aliased `cv` was diagnosed
correctly.

**The `fix` names a candidate when there is one** (127.122). When the misspelled
member is within edit distance 2 of a real export of that same alias, and
exactly one export is that close, the fix reads `did you mean 'str.concat'?`
instead of the generic text. All four guards matter and are deliberate: the
candidate is always a recorded export rather than a name invented from the
spelling; a tie between two equally close exports means the compiler does not
know which was meant and keeps the generic text; and names shorter than four
characters are not scored at all. AGENTS.md §3.1 forbids a `fix` that is a
guess.

**Fix:** Check the module's interface for the correct member name.

### E4032

**Cannot interpolate a composite value into a string**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check (also emitted by codegen) |

Emitted when a `\(expr)` interpolates an array, struct, or map. Convert it to a string first (e.g. its fields/elements). Fires at code generation.

### E4033

**A type name used where a value is required**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

Emitted when a declared type's name stands in a value position. Four spellings
reach it, from three stages:

- **`$Type.member`** (typecheck, 127.90) — member access on a type rather than
  an instance. There is no value to take a field of, so the expression has no
  meaning. Before 127.90 it type-checked and lowered to a field load off a null
  base, producing a plausible-looking `0`.
- **a bare `Type` in any value position** (name resolution, 137.4) — a call
  argument, the right-hand side of a `let`, an operand, a condition, a
  return value, or a field-access base. toke keeps type names and value names
  in one namespace, and `resolve_ident` used to accept whatever the scope
  lookup returned without inspecting its kind, so these resolved clean,
  type-checked clean, and failed at clang as `use of undefined value '%Type'`
  — naming an LLVM temporary rather than the program. In the 137 migration
  this hid a genuinely missing parameter and three functions referencing a
  binding declared nowhere.
- **`keyword.member(...)` on a PRIMITIVE type keyword** (typecheck, 127.135) —
  `str`, `i64`, `u64`, `f64`, `bool` and `void`, when no module is imported
  under that alias and no binding of that name is in scope. The bullet above
  keys on a user `DECL_TYPE`; the primitives are seeded as `DECL_PREDEFINED`
  and so were exempt from it. The commonest way in is a forgotten import:
  `str.concat("a";"b")` without `i=str:std.str;` passed `--check` **and**
  `--lint` in silence and then failed at clang as `use of undefined value
  '%str'`, naming a temporary `.ll` file the author never saw. For `str` the
  `fix` names the import, because that repair is deterministic. For the
  others no module of that name exists, so the field is absent (§3.1).

  Note the shape of the old hole: whether a diagnostic appeared depended on
  whether the member was **real**. `str.nosuchmember(...)` already produced a
  clean `E9004` naming the missing import, so the one case with no diagnostic
  at all was the correct-looking mistake — calling a real member of a module
  you forgot to import.

- **a type as a key or value of a map literal entry** (typecheck, 127.226) —
  `@($str:1)`, `@("a":$str)`, or a type entry alongside real ones. The one
  legitimate shape is the typed EMPTY map, a single entry with a type as its
  key and, as its value, a type or another typed-empty literal:
  `@($str:$i64)`, `@($str:@($tok))` — the map counterpart of the typed-empty
  array `@($f64)`. Before 127.226 every one of these built a map holding a
  junk entry keyed by `""`, so the empty form answered `.len` 1. No `fix`: the
  checker cannot tell whether an empty map or a real key was meant.

A name that is not also a type is still `E3011` (undefined identifier), and
the legitimate bare-type positions — `as Type` casts, `$Type{…}` struct
literals, `:Type` annotations, sum-type variant constructors and module
aliases — are unaffected.

<!-- skip-check -->
```text
m=test;
t=thing{n:i64};
f=take(v:i64):i64{<v};
f=main():i64{ <take(thing) };
```

Triggers E4033 on `thing`.

**Fix:** Bind an instance of that type and use the binding.

### E4034

**Field access on a layout the compiler cannot establish**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check (emitted by codegen) |

Emitted when a `.field` access reaches lowering and the base's struct type is not established, and the field name does not resolve to one unambiguous layout — either no struct in scope declares it, or several declare it at different offsets.

Codegen used to answer both cases with a guess that looked like data: `struct_field_index()` returns `0` for "not found", and an unresolved base took the first registered struct declaring a field of that name. That is how a 512-byte secure buffer reported its size as `51` (127.86), a nonexistent field returned the first field (127.89), and a documented field read landed in another struct's slot (136.28).

No `fix` is supplied: the obvious instruction — annotate the binding — is not correct for every shape.

### E4035

**Map has no property with that name**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check (also emitted by codegen) |

Emitted for `m.name` on a map where `name` is not one of the two map
properties, `len` and `keys`. A map has no fields, so there is no layout to
resolve `name` against.

Before 127.106 the checker returned "unknown" for such an access and said
nothing, after which lowering took the struct-field path with no layout
established, `struct_field_index()` returned its `0` fallback, and the program
loaded word 0 of the map's runtime block and printed it. `m.values` — the
spelling that was reported — came out as a pointer rendered as a decimal at
exit 0 with no diagnostic; `m.size`, `m.count`, `m.entries`, `m.items` and any
misspelling were the same undefined read. The value that a given build
happened to print was not stable, which is why the earlier record of this
defect as a loud runtime trap and the later record of it as silent wrong output
are two observations of one bug rather than a regression between them.

The checker reports it, so `tkc --check` sees it. Lowering reports it too, for
a map whose type the checker cannot recover — `let m2 = m.set(k;v)` is typed by
the backend's map tag and by nothing in the checker.

No `fix` is supplied. There is no deterministic repair: `values` has no method
form and no runtime support, so "add the parentheses" would be wrong — the
lesson of 131.78 — and a did-you-mean for an arbitrary misspelling is a guess.
The two property names that do exist are reported in `expected`.

Note that `m.values()`, the CALL form, is E4036 rather than this diagnostic:
it is a member call, and the receiver dispatch is what refuses it.

### E4036

**Receiver has no method with that name**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

Emitted for `recv.member(args)` where `recv` is a value rather than an import
alias and `member` is not a member the receiver dispatch lowers.

Until 127.115 this was checked by nothing at all. The type checker reads a
call's callee name from the callee node's own token; for a field-access callee
that token is the `.`, so the module-scope lookup failed, the call fell into
the unresolved branch, and both checks that live there are gated on the base
resolving to an imported module — which it cannot, because the base is a
value. Code generation then emitted a call to a **bare, unmangled** symbol
named after the member. A toke function is mangled `<module>_<name>`, so that
symbol never existed:

```
f=pair(a:str;b:str):str{ ... };
let y = x.pair("A");        (* correct arity *)
```

passed `tkc --check` at exit 0 and then failed at `ld` with `Undefined
symbols: "_pair"`. So did `x.pair()` and `x.pair("A";"B")` — the arity was
never the point, because the receiver path does not resolve user functions at
all. toke has no uniform-function-call syntax: `x.pair(a)` is not `pair(x;a)`.
The same fallback swallowed `a.nosuch()` on an array, `s.nosuch()` on a
string, `m.values()` on a map, `p.nosuch()` on a user-declared struct, and any
member call on a receiver the checker could not type.

The set of members the receiver dispatch lowers is a table in the code
generator, and it is the gate the emitter itself dispatches on, so the checker
and the emitter cannot disagree about which member names exist.

The `fix` is **conditional** (127.153). In general there is no deterministic
repair: the member may be a misspelling, a method that exists on no receiver
type, or a function the author meant to call directly, and a did-you-mean for
an arbitrary name is a guess (the 127.106 / 131.78 lesson). In those cases the
field is absent, and when a function of that name is declared in the module
the message says so — a statement of fact, not an instruction.

One case is not a guess: a module function of that name whose **declared arity
equals the receiver plus the supplied arguments**. The direct call is then
guaranteed well-formed, the receiver is a genuine candidate first argument,
and toke admits no other transformation — so the field is filled, and it
states the two numbers that justify it so a consumer can check the reasoning
rather than trust it. Where the arities disagree the rewrite would not
compile, and the field stays absent.

Note what is still refused: filling this field *unconditionally* with
`pair(x;"A")`. The compiler knows such a function exists; it does not know it
was meant. toke has no uniform-function-call syntax, so `x.pair(a)` and
`pair(x;a)` share a name and nothing else.

Related: E4027 is the module-member form of the same question
(`str.nosuchfn(...)`), and E4026 is raised instead when the member does exist
and the call disagrees with its arity.

### E4070

**Assignment to immutable binding**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

Emitted when code assigns to a `let` binding that was not declared mutable.

### E4071

**Function body ends in a value expression with no `<`**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

`<` is toke's return operator. A function body that ends in a *value*
expression without one computes the value, discards it, and returns `0` —
so this is a hard error, not an implicit tail return.

<!-- skip-check -->
```text
m=test;
f=arith(): i64 { 1+2+3 };
```

Until 127.113 that program compiled clean, `--lint` said nothing, and
`arith()` returned **0** while `<1+2+3` returned **6**. The IR showed the
shape exactly: the exit block loaded the computed value and then threw it
away — `%t16 = load i64, i64* %t0` followed by `ret i64 0 ; implicit return`.
The same held for `if(x>0){7}el{9}`, for a tail `mt`, and for `str`. 56.10.2
diagnosed this in 2026-05 for pointers ("all content items stored as NULL
pointers"), worked around it at the call site with `let r=...; <r`, and
logged the codegen bug; this is the diagnostic that should have been written
then.

The repair is to say what the function returns:

<!-- skip-check -->
```text
m=test;
f=arith(): i64 { <1+2+3 };
f=tailif(x: i64): i64 { if(x>0){<7}el{<9} };
```

**Scope.** The check fires only on a discarded *value*, where the silent `0`
is a wrong answer:

* the function's return type is neither `void` nor unresolved, **and**
* the last statement of the body is an expression statement, a complete
  `if`/`el`, or a `mt`, **and**
* that statement evaluates to a value — either a type the checker resolved,
  or an expression whose only possible effect is to compute one, **or** it
  is a `!` propagation whose type the checker could not resolve (127.127:
  `(file.listall("")!$storeerr)` ends the body with nothing returned on the
  success path, so the caller receives the null filler — measured rc=139).
  A propagation the checker proves `void` is a missing return, not this
  diagnostic.

A tail call returning `void` (`io.println(..)`) discards nothing and is not
this diagnostic. Neither is a non-void function that simply never returns at
all; that is a separate defect.

**`fix` field.** Populated only where prefixing `<` is the sole reading the
source can have: arithmetic, a literal, a binding, a field or index read, a
cast, or a composite literal. It is deliberately **absent** for

* a **call** — the result may be discarded on purpose, and `<` would change
  behaviour rather than restore it;
* a **`!` propagation** — it reads as error handling, not a return value;
* an **`if`/`mt`** — the repair is a `<` inside each arm, not one in front of
  the construct, and the two are not interchangeable.

Per `AGENTS.md` §3.1 an incorrect `fix` breaks the automated repair loop, and
131.78 is the standing example of an "obvious" suggested fix that was wrong.

### E4072

**Ambiguous variant constructor**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

`<$v(x)` returns a variant constructed by the call form, and `$v` is declared
by two or more sum types of which none is the enclosing function's error
type. The compiler resolves the call form to the function's error type if
that declares the variant, else to the one sum type that does; with several
candidates and no error-type match there is nothing to choose by, and the
payload was returned untagged (story 127.59: measured rc=139 when the caller
matched on it).

<!-- skip-check -->
```text
m=test;
t=$a{$v:i64;$w:i64};
t=$b{$v:str};
f=k(x: i64): $a { <$v(x) };
```

Write the qualified literal instead — `<$a{$v:x}`. There is no `fix` field:
which sum type was meant is not decidable from the source.

The single-owner case — `$v` declared by exactly one sum type that is neither
the function's return type nor its error type — is a type mismatch and is
reported as **E4031**, the code the literal form `<$other{$v:x}` already drew.

### E4073

**Nested interpolation the interpolation lowering cannot delimit**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

A `\(..)` interpolation whose expression contains another interpolated
string. The lexer accepts it, but the lowering finds the end of each `\(..)`
by counting parens while stepping over escape pairs, so the `(` of the inner
`\(` is not counted and its `)` is: it would compile a slice that stops
short. Until 127.47 this passed `--check` and failed the build with
`E2002`/`E2004` reported at an offset inside the compiler's own wrapper
buffer rather than the real line.

<!-- skip-check -->
```text
m=test;
f=tag(s: $str): $str { <"<\(s)>" };
f=show(n: i64): $str { <"\(tag("\(n)"))" };
```

Bind the inner string first — `let t="\(n)"; <"\(tag(t))"`. There is no
`fix` field: lifting the inner string ahead of the statement can reorder the
side effects of the other interpolations in the same literal.

### E4074

**A comparison stands as a statement: its result is discarded**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check |

A comparison (`==`, `!=`, `<`, `>`, `<=`, `>=`) written as a statement
computes a `bool` and throws it away, so the statement does nothing. The
usual cause is `==` written for the assignment `=`: until 137.14 / 138.10
`x.a==9;` compiled as a silent no-op. `<` is also toke's return operator, so
a missing `;` can turn `g()` and `<$ok(1)` into one comparison. The last
statement of a block is not judged (it may be the block's value; a function
body's tail is E4071's). There is no `fix`: `=` is the likely repair for
`==`, and not for the other operators.

<!-- skip-check -->
```text
m=test;
t=$r{a:i64};
f=main(): i64 { let x=mut.$r{a:1}; x.a==9; <x.a };
```

## Arena Errors (E5xxx)

### E4075

**Regex pattern literal is not in the supported regex language**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | type_check (emitted by the regex_check pass that follows it) |

A string-literal pattern passed to `str.containsre`, `str.countre`,
`str.findall`, `str.matches` or `str.replacere` uses syntax outside toke's
regex language — a lazy quantifier (`*?`), a backreference (`\1`),
lookaround or an inline flag other than a leading `(?i)`, an unknown escape,
an unbalanced `(`, `)` or `[`, a `{` that does not start a bound, an empty
alternative, or repeats that expand past 2000 states. The diagnostic points
at the offending byte inside the literal. Until 137.9 these patterns
compiled and then matched nothing at run time, because a POSIX `regcomp`
failure read as "no match". The supported language is in
[str.md](/docs/stdlib/str/#regular-expressions); the same engine checks the
pattern here and runs it in the program, so they cannot disagree. A pattern
held by an immutable `let` bound to a string literal is checked the same way
(121.45) when that `let` is provably the binding the call reads: it is the
name's only binder in the function, and the call follows it in its block. A
pattern that is shadowed, `mut.`-bound, a parameter, or computed at run time
is checked when it is used, and an invalid one traps `RT008`.

<!-- skip-check -->
```text
m=test;
i=str:std.str;
f=main():i64{ <str.countre("<a><b>";"<.*?>") };
```

Rewrite the construct, e.g. `<[^>]*>` for `<.*?>`. There is no `fix` field:
the intended pattern is not knowable from the rejected one.

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
| Stage    | arena_check |

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

The clang/linker sub-process returned a non-zero exit status. clang's own
output is printed first, unchanged; the record follows it.

When the failure can be traced to a toke declaration, the message ends
`— while compiling f=<name> at <file>:<line>` (or `closure at <file>:<line>`
for a lifted closure), and the record's `file` and `pos` are that
declaration's. It is traced when clang reports a line of the emitted IR (the
line is looked up in the `define` that contains it), or when the linker
reports an undefined symbol (looked up in the first function whose IR
references it; if the symbol is itself a toke extern, the message also says
where it was declared). The message also carries clang's first error text or
the undefined symbol's name. A failure with no such trace — an unwritable
output path, a missing C library — stays at line 0 (137.13).

When clang itself could not be started (not installed, or not on `PATH`),
the message says `clang not found on PATH` and names the remedy instead of
reporting exit code 127: tkc runs clang to compile and link every program.

### E9004

**Unresolved stdlib call — missing import**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

A `module.fn()` call refers to a stdlib module that was not imported.

### E9005

**Match on a value whose sum type is not established**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

A `mt` of three or more arms was written over a value whose sum type the
compiler could not determine. Two arms lower to the result-match form
(`$ok`/`$err`), which dispatches once on the error flag; three or more arms
need the value's sum type so the tag can be switched on. Annotate the matched
value with its sum type, or match it inside a function that takes it as a
typed parameter.

Before story 137.2 this shape emitted LLVM IR with a duplicated basic-block
label and was rejected by clang with "Terminator found in the middle of a
basic block!" — a backend crash rather than a diagnostic.

### E9010

**Compiler limit exceeded**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

A fixed compiler capacity was exceeded (e.g. too many locals, functions, or imports).

The import, function-signature and struct-type caps are met at `--check`, at
the import or declaration that crosses them, with `expected` = the limit and
`got` = the count. Imported signatures and types count: every function of an
imported user module's `.tki`, every record of an imported `std` module, and
the built-in `timeparts` and `none` types. The message lists what was counted.

**Fix:** raise the limit to the count: `--max-imports=N` / `max_imports`,
`--max-funcs=N` / `max_funcs`, `--max-structs=N` / `max_structs` (in
`tkc.toml`). The `fix` field names the exact value (story 138.15). The
defaults are 32 imports, 1024 function signatures and 1024 struct types
(`tkc --show-limits`); the last two were 128 and 64 until story 127.250.

A cap is a count: `0` allows none. Caps so large (or negative) that a
registry sized from them cannot be one compiler allocation are refused at
`--check` too, with the same E9010 as the build, naming the caps; lower them
(story 127.264).

The per-function local-variable cap (`--max-locals=N` / `max_locals`, shared
by pointer locals, local types and shadowing aliases) is reported once per
function, closure or globals initialiser that crosses it, at that function,
naming it, with `expected` = the limit and `got` = the count needed. That
count is a lower bound -- a full registry stops recording, which can hide
further demand -- so this E9010 carries no `fix` (story 127.252). `--check`
meets the part of it that is certain: the parameters, `let` bindings and
`lp(let ..)` variables of a function's own body (match-arm subtrees and
closure bodies are left to codegen, which may release or lift them), so a
function the checker passes can still be refused by the build, never the
reverse (127.252 residue).

Source nested too deeply for the compiler's recursive passes is refused
before them, as one E9010 at the offending token or node, no `fix`: more
than 256 open brackets, or a run of more than 256 prefix operators
(`-` `!` `~`) or type prefixes (`@` `*`), or a syntax tree more than 1000
levels high -- a long `a+b+c+...` chain is one level per operator. Before
story 121.28 such input crashed the compiler with no diagnostic.

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

### E9022

**Stdlib directory not found**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

The compiler could not find its standard library (a directory holding
`tk_runtime.c`). It looks, in order, at `TKC_STDLIB_DIR` from the environment
(which, when set, must be such a directory), the path built into the compiler
if that path is absolute, and the paths beside the `tkc` executable
(`<dir>/src/stdlib`, `<dir>/../src/stdlib`, or the built-in path taken
relative to `<dir>`). Emitted once, before any source is read, instead of the
unrelated undeclared-identifier errors a missing stdlib used to cause.
Set `TKC_STDLIB_DIR` to the `src/stdlib` directory of a toke checkout or
release (127.179).

### E9023

**No main function defined**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

A build to an executable was requested for a program that declares no
`f=main()`. Emitted before any IR is written, instead of the linker's
"`_main` not found". Declare `f=main():i64{...};`, or use `--check` to
type-check a library module that has no entry point. The diagnostic carries
no `fix`: the only mechanical fix, an empty `main`, builds a program that does
nothing.

Before 127.208 this condition was reported as E9020, a code whose meaning is
"failed to write .tkir file"; E9020 now means only that.

### E9024

**Stdlib or runtime directory path contains whitespace**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

A build to an executable was requested while `TKC_STDLIB_DIR` (as resolved by
the compiler, see E9022) or `TKC_RUNTIME_DIR` names a directory whose path
contains whitespace. The stdlib C sources and vendor include flags reach
clang as whitespace-separated arguments, so such a path would be split into
fragments — and a fragment that looks like a flag (`-v`, `-o`) would reach
clang as one. Emitted before clang is run; `--check` is unaffected. Use a
directory whose path has no whitespace (a symlink to it works: the compiler
resolves `TKC_STDLIB_DIR` to its real path). No `fix`: the right path is not
knowable from the diagnostic (121.28, audit-120 compiler-toolchain COM-02).

### E9026

**`--migrate` declined; source echoed unchanged**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen |

`tkc --migrate` did not rewrite its input. The source is written to stdout
byte for byte, so the redirect `--migrate` is used through keeps the
original, and the compiler exits **4** (`TKC_EXIT_DECLINED`) rather than 1, so a
migration driver can tell "declined, nothing written but the input" from a
failed run (story 127.197; both declines used to print plain stderr and exit
1). The message names the reason:

- the source **already compiles** under the current profile, so there is
  nothing to migrate (127.57); the record has no line;
- its **comments cannot be kept** (127.161). `--migrate` normally keeps every
  comment (spec v0.4 §H); it declines when it cannot place one safely, and
  the message says why: a `(*` that is never closed, a `//` or `#` comment
  whose text holds `(*` or `*)` (it cannot become a `(* *)` comment with the
  same extent), a rewrite that moved code across lines, or a source that
  needed the migrator's second pass. The record's line is the comment
  concerned, or 0 when the cause is the whole file.

`TKC_MIGRATE_FORCE=1` migrates anyway, dropping every comment. No `fix`:
whether to do that is the user's choice.

## Package Resolution (E9011--E9014, E9025, W9010 -- reserved)

The codes `docs/reference/package-manifest.md` gives for package resolution.
They are defined in `src/pkg.h` so that no compiler diagnostic takes one, but
the resolver is not wired into a `tkc` command and nothing emits them yet
(story 121.50). The unsatisfiable-constraints condition was documented as
E9010 until then -- the compiler's "compiler limit exceeded" -- and moved to
E9025 (E9024 went to the compiler in the meantime, 121.28).

### E9011

**Package not found in the index and no git URL**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen (reserved for package resolution; not yet emitted) |

A dependency names a package the central index does not have, and gives no `git` source.

### E9012

**Malformed pkg.toml**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen (reserved for package resolution; not yet emitted) |

`pkg.toml` is missing a required field, is not the TOML subset the manifest allows, uses an unsupported constraint, names a dependency twice, or has an unknown table or key.

### E9013

**Git clone or fetch of a dependency failed**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen (reserved for package resolution; not yet emitted) |

The repository of a dependency could not be cloned or fetched.

### E9014

**No git tag matches the resolved version**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen (reserved for package resolution; not yet emitted) |

A dependency's repository has no tag for the version resolution selected.

### E9025

**No version of a dependency satisfies every constraint**

| Field    | Value |
|----------|-------|
| Severity | error |
| Stage    | codegen (reserved for package resolution; not yet emitted) |

Minimal version selection took the highest minimum any constraint names, and some constraint on that package does not admit it (an exact version it does not name, or an upper bound it reaches).

### W9010

**pkg.lock is out of date relative to pkg.toml**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | codegen (reserved for package resolution; not yet emitted) |

`pkg.toml` changed after `pkg.lock` was written.

## Semantic Warnings (W2xxx / W3xxx / W5xxx / W8xxx)

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

### W3013

**`let` shadows a binding in an enclosing scope**

| Field    | Value           |
|----------|-----------------|
| Severity | warning         |
| Stage    | name_resolution |

A `let` (or `let mut`, or a `lp` loop variable) introduces a name that is
already bound by an enclosing scope — an outer block, or a parameter.

This is **legal**: story 75.1.7 chose "allow same-scope and cross-scope
shadowing", and §11.8.1 of the spec publishes `let x = x + 1;` as a valid
program. The warning exists because the same source also reads exactly like
an assignment that it is not: a `let x = …` inside an `if` or a `lp` body
declares a *second* x, and the outer one is unchanged when the block ends.

Same-scope shadowing — the spec's own example, where the two bindings are
adjacent and visibly sequential — does **not** warn.

<!-- skip-check -->
```text
m=test;
f=bad(n: i64): i64 { let x = 1; if(n>0){ let x = 2; }; < x };
```

Triggers W3013 on the inner `let`; the function returns 1.

**Fix:** If an assignment was meant, declare the outer binding `let x =
mut.1` and write `x = 2`. If a distinct binding was meant, rename it.

---

### W5001

**Value may escape arena scope**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | arena_check (emitted by the type checker) |

Escape analysis warns that a value may outlive its arena.

### W8001

**Extern function declaration — FFI unsafe**

| Field    | Value |
|----------|-------|
| Severity | warning |
| Stage    | ffi (emitted by codegen) |

An `extern` function crosses the FFI boundary and is not memory-safe.
