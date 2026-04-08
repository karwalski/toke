# toke lint rules — v1

## Introduction

`toke lint` is a standalone static analysis tool that runs on toke source files after a successful parse and type-check pass. It is distinct from the compiler's `--check` flag and from the LSP error diagnostics: the compiler rejects code that is structurally or type-theoretically wrong; the linter flags code that is legal but likely to cause problems, hard to read, or contrary to toke convention.

Rules are grouped into three categories:

| Category | Meaning |
|---|---|
| **error** | Code the compiler currently accepts but that will fail or behave incorrectly at runtime. |
| **warning** | Code that is valid but violates toke style or introduces maintenance hazards. |
| **hint** | Code that is non-idiomatic or potentially confusing, but entirely valid. No action required. |

Rules are referenced by a short kebab-case identifier (e.g. `unused-let`). The `toke lint` CLI output format is:

```
file.tk:line:col: [rule-id] message
```

Exit codes: `0` = no findings, `1` = findings present, `2` = parse or type-check error (lint cannot run).

---

## Rule ID reference table

| Rule ID | Category | Description |
|---|---|---|
| `unused-let` | warning | A `let` binding whose value is never read |
| `unused-import` | warning | An `i=` import alias that is never referenced |
| `redundant-bind` | warning | A `let` binding of a variable to itself (`let x = x`) |
| `unreachable-code` | error | Code after a `<` (return) statement in the same block |
| `fn-name-convention` | hint | Function name uses non-camelCase style |
| `type-name-convention` | hint | Type name is not PascalCase |
| `keyword-prefix-ident` | hint | Identifier begins with a keyword prefix, confusing to human readers |
| `empty-fn-body` | warning | Function body is `{}` with no return value and no placeholder annotation |
| `mutable-never-mutated` | warning | A binding declared `mut` is never reassigned |
| `error-result-ignored` | warning | A call returning a `!`-error result type whose error case is never handled |
| `struct-field-shadow` | hint | A `let` binding inside a function uses the same name as a struct field being operated on in the same scope |

---

## Rules

---

### `unused-let`

```yaml
id: unused-let
category: warning
fixable: yes
```

**Description:** A `let` binding whose value is never read.

**Rationale:** An unused binding is almost always a mistake — either a value was computed and then forgotten, or a refactor left a dead assignment behind. Unlike many languages, toke has no `_` prefix convention for intentionally-unused bindings; if a value is not needed it should not be bound.

**Example violation:**

```toke
m=demo;
f=main():i64{
  let x=42;
  <0
};
```

`x` is bound but never read before the function returns.

**Example fix:**

```toke
m=demo;
f=main():i64{
  <0
};
```

Remove the unused binding entirely.

**Auto-fix (`toke lint --fix`):** Yes. The `let` statement is removed. The fix is applied only when the right-hand side expression has no side effects (i.e. is a literal, a variable reference, or a pure arithmetic expression). If the right-hand side is a function call, the binding is rewritten to a bare call statement rather than removed, because the call may have side effects.

**Edge cases:**

- A `let` whose value flows into a subsequent `mut` reassignment chain counts as read — do not flag it.
- A `let` inside a branch that is provably unreachable is flagged by `unreachable-code` first; do not double-flag.
- In `void` functions the binding is also flagged; the fix is the same.

---

### `unused-import`

```yaml
id: unused-import
category: warning
fixable: yes
```

**Description:** An `i=` import alias that is never referenced in the module body.

**Rationale:** Unused imports increase compile-time linking overhead (when `--fix` is not used), mislead readers about the module's dependencies, and become dead weight during refactors.

**Example violation:**

```toke
m=demo;
i=http:std.http;
i=json:std.json;

f=main():i64{
  http.serve(8080);
  <0
};
```

`json` is imported but never used.

**Example fix:**

```toke
m=demo;
i=http:std.http;

f=main():i64{
  http.serve(8080);
  <0
};
```

**Auto-fix (`toke lint --fix`):** Yes. The `i=` declaration line is removed.

**Edge cases:**

- If an import alias appears only in a comment companion file (`.md`), it is still flagged — toke has no source-level comments, so the alias is unused from the compiler's perspective.
- Wildcard or re-exported imports (if supported in future versions) are exempt.

---

### `redundant-bind`

```yaml
id: redundant-bind
category: warning
fixable: yes
```

**Description:** A `let` binding where the right-hand side is the same identifier as the left-hand side (`let x = x`).

**Rationale:** `let x = x` is always a no-op. It most commonly appears as a copy-paste error or after a refactor renames one side but not the other. There is no toke construct where this is intentionally meaningful.

**Example violation:**

```toke
m=demo;
f=process(x:i64):i64{
  let x=x;
  <x*2
};
```

**Example fix:**

```toke
m=demo;
f=process(x:i64):i64{
  <x*2
};
```

**Auto-fix (`toke lint --fix`):** Yes. The statement is removed unconditionally because it cannot have side effects.

**Edge cases:**

- `let x = mut.x` is not redundant if mutability is intentionally being stripped or added; do not flag.
- Only exact same-name matches are flagged. `let x = y` where `y` happens to equal `x` at runtime is not detectable statically.

---

### `unreachable-code`

```yaml
id: unreachable-code
category: error
fixable: no
```

**Description:** One or more statements appear after a `<` (return) statement in the same block and can never execute.

**Rationale:** Code after a return is dead. It will never run, wastes space in the binary, and typically indicates a logic error — a missing conditional or a misplaced return. Unlike warnings, this is categorised as **error** because the programmer's intent is almost certainly not expressed by the code as written, and the dead statements may shadow a bug.

**Example violation:**

```toke
m=demo;
f=compute(n:i64):i64{
  <n*2;
  let result=n+1;
  <result
};
```

`let result=n+1` and the second `<result` are unreachable.

**Example fix:**

```toke
m=demo;
f=compute(n:i64):i64{
  <n*2
};
```

Remove or reorder the dead statements.

**Auto-fix (`toke lint --fix`):** No. The correct fix depends on intent — either the early return is wrong, or the later statements are wrong. The linter cannot determine which.

**Edge cases:**

- Only flags within the same syntactic block `{}`. A `<` in one branch of an `if`/`el` does not make statements in the other branch unreachable.
- Multiple returns in separate branches are fine and are not flagged.
- A `<` as the last statement of a block followed by the closing `}` is not flagged (trivially correct).

---

### `fn-name-convention`

```yaml
id: fn-name-convention
category: hint
fixable: no
```

**Description:** A function name uses non-camelCase style. toke convention for user-defined functions is camelCase.

**Rationale:** The toke standard library uses camelCase for all exported function names (e.g. `str.fromInt`, `http.serveWorkers`). Consistent naming across user code and stdlib makes the language easier to read and means tooling (autocomplete, documentation generators) can present a uniform interface. Snake_case function names stand out as alien in a toke codebase.

**Example violation:**

```toke
m=demo;
f=compute_total(n:i64):i64{
  <n*10
};
```

`compute_total` uses snake_case.

**Example fix:**

```toke
m=demo;
f=computeTotal(n:i64):i64{
  <n*10
};
```

**Auto-fix (`toke lint --fix`):** No. Renaming a function requires updating all call sites, which may span multiple files. The linter cannot safely rename across a project boundary in v1.

**Edge cases:**

- `main` is exempt. It is the required entry point and is always lowercase.
- Single-word function names (e.g. `parse`, `send`) conform regardless of case.
- Functions exposed as foreign-function interface (FFI) exports, if applicable, are exempt because external naming is not under toke's control.
- The `_` placeholder name (if used as a stub) is exempt.

---

### `type-name-convention`

```yaml
id: type-name-convention
category: hint
fixable: no
```

**Description:** A user-defined type name is not PascalCase.

**Rationale:** toke uses `$PascalCase` for user-defined struct types (e.g. `t=$UserRecord{...}`). PascalCase is the visual signal that a `$`-prefixed identifier is a named type rather than a variable. Lowercase or snake_case type names make code harder to scan.

**Example violation:**

```toke
m=demo;
t=$user_record{name:str;age:i64};
```

`user_record` uses snake_case instead of PascalCase.

**Example fix:**

```toke
m=demo;
t=$UserRecord{name:str;age:i64};
```

**Auto-fix (`toke lint --fix`):** No. Same cross-file rename risk as `fn-name-convention`.

**Edge cases:**

- Built-in primitive type names (`i64`, `u64`, `str`, `bool`, `void`, `f64`, etc.) are not flagged — they are defined by the language, not the user.
- Stdlib-defined types (`$req`, `$res`, `$httperr`, etc.) are not flagged — the user does not control stdlib naming.
- Single-word all-lowercase names that are commonly used as opaque type aliases (e.g. `t=$token{...}`) are flagged; the intent is always PascalCase (`$Token`).

---

### `keyword-prefix-ident`

```yaml
id: keyword-prefix-ident
category: hint
fixable: no
```

**Description:** An identifier begins with a toke keyword as a prefix, making it visually ambiguous to human readers even though the compiler resolves it unambiguously via longest-match tokenisation.

**Rationale:** toke's lexer uses longest-match: `letfoo` is always the single identifier `letfoo`, never `let` + `foo`. This is correct and unambiguous to the compiler. However, human readers who glance at `letfoo = 5` may initially misparse it as a `let` binding — particularly when reading code quickly or in contexts with poor syntax highlighting. This hint exists to surface names that are likely to cause reader confusion.

Keyword prefixes to flag: `let`, `mut`, `fn`, `if`, `el`, `use`, `import`, `return`, `as`, `true`, `false`, `void`.

**Example violation:**

```toke
m=demo;
f=main():i64{
  let mutcount=mut.0;
  mutcount=mutcount+1;
  <mutcount
};
```

`mutcount` starts with the keyword `mut`.

**Example fix:**

```toke
m=demo;
f=main():i64{
  let count=mut.0;
  count=count+1;
  <count
};
```

**Auto-fix (`toke lint --fix`):** No. Renaming is not mechanically safe.

**Edge cases:**

- Only alphanumeric continuations are flagged. `let` followed by `_` or a non-keyword character sequence is not a realistic source of confusion and is not flagged.
- The prefix must be an exact keyword match: `letter` starts with `let` and `ter` is not a keyword, so `letter` is flagged. `leet` starts with `l` only and is not flagged (no keyword `l` exists).
- Function parameter names, `let`-bound names, and `t=` type names are all in scope for this rule.
- This rule connects to Epic 44 (whitespace semantics clarification); see `44.1.4` for the related "did you mean?" compiler diagnostic proposal.

---

### `empty-fn-body`

```yaml
id: empty-fn-body
category: warning
fixable: no
```

**Description:** A function has an empty body (`{}`) that returns no value and carries no recognised placeholder annotation.

**Rationale:** An empty function body is almost never intentional in finished code. It most commonly appears as a stub left over from scaffolding. In a `void` function an empty body silently does nothing; in a non-`void` function it is a type error the compiler will catch. The warning targets the `void` case — where the compiler is silent but the code is almost certainly incomplete.

**Example violation:**

```toke
m=demo;
f=onConnect():void{};
```

`onConnect` has an empty body and does nothing.

**Example fix — if intentional stub:**

```toke
m=demo;
f=onConnect():void{
  let _todo=0
};
```

Add a visible placeholder to signal intent, suppressing the lint warning.

**Example fix — if the body is missing:**

```toke
m=demo;
f=onConnect():void{
  log.info("client connected")
};
```

**Auto-fix (`toke lint --fix`):** No. Whether the empty body is intentional (stub) or accidental (missing implementation) cannot be determined mechanically.

**Edge cases:**

- Functions with return type `void` are the primary target. A non-`void` empty body is already a compiler error and is not double-flagged by the linter.
- If the function body contains only whitespace but no statements, it is treated as empty for this rule's purposes.
- A function with a single `let _todo=0` or similar recognisable stub pattern is not flagged — the presence of any statement signals intent.

---

### `mutable-never-mutated`

```yaml
id: mutable-never-mutated
category: warning
fixable: yes
```

**Description:** A binding declared with `mut.` is never reassigned after its initial declaration.

**Rationale:** In toke, `let x=mut.10` explicitly opts into mutability. If the binding is never subsequently reassigned, the `mut.` annotation is pointless overhead — it misleads readers into expecting mutation that never happens. Removing it makes intent clearer and enables the compiler to apply immutability optimisations.

**Example violation:**

```toke
m=demo;
f=main():i64{
  let x=mut.42;
  <x
};
```

`x` is declared mutable but only read, never reassigned.

**Example fix:**

```toke
m=demo;
f=main():i64{
  let x=42;
  <x
};
```

**Auto-fix (`toke lint --fix`):** Yes. The `mut.` prefix is stripped from the initial value expression. Safe because no reassignment exists to break.

**Edge cases:**

- Only direct reassignment (`x=newvalue`) counts as mutation. Passing `x` to a function that internally modifies it via a pointer (if such a pattern exists in toke) would also count — flag conservatively: if any mutation is possible through aliasing, do not flag.
- Bindings that flow into a branch where mutation is conditional are not flagged, because the static analysis cannot prove the branch is never taken.

---

### `error-result-ignored`

```yaml
id: error-result-ignored
category: warning
fixable: no
```

**Description:** A function call whose return type includes an error variant (`T!E`) is used in a context where the error case is never inspected or propagated.

**Rationale:** toke stdlib functions signal failure via `!`-error result types (e.g. `str.slice` returns `str!SliceErr`). If the caller ignores the error case — for example, by binding the result directly to a `let` without any error handling — a runtime error will propagate silently or produce a panic. Explicit error handling is the convention; ignoring errors is almost always a bug.

**Example violation:**

```toke
m=demo;
i=str:std.str;
f=main():i64{
  let s=str.slice("hello";1;3);
  <0
};
```

`str.slice` returns `str!SliceErr` but the result is bound as if it were a plain `str`, with no handling of `SliceErr`.

**Example fix:**

```toke
m=demo;
i=str:std.str;
f=main():i64{
  let s=str.slice("hello";1;3);
  if(s.isErr){
    <1
  };
  <0
};
```

Check the error case explicitly before using the value.

**Auto-fix (`toke lint --fix`):** No. Error handling strategy (propagate, panic, default value) is context-dependent and cannot be inferred mechanically.

**Edge cases:**

- If the result is immediately passed to another function that accepts the full `T!E` type, the error is not ignored — do not flag.
- Functions that return `void!E` where the `void` case is discarded (bare call statement) are flagged for the unhandled error, not for the discarded void.
- This rule requires type information from the type-checker pass; it cannot run on parse output alone.

---

### `struct-field-shadow`

```yaml
id: struct-field-shadow
category: hint
fixable: no
```

**Description:** A `let` binding inside a function uses the same name as a field of a struct type that is actively being constructed or destructured in the same scope.

**Rationale:** When a function receives a struct parameter `a:$Vec2` with field `x` and later writes `let x=...`, the local `x` shadows the conceptual `a.x` that readers expect to see referenced. This is not an error — `x` and `a.x` are distinct — but it increases the cognitive load of reading the function and is a common source of bugs when the wrong `x` is used in an expression.

**Example violation:**

```toke
m=demo;
t=$Vec2{x:i64;y:i64};
f=scale(a:$Vec2;factor:i64):$Vec2{
  let x=a.x*factor;
  let y=a.y*factor;
  <$Vec2{x:x;y:y}
};
```

`let x` and `let y` shadow the field names of `$Vec2`, which is being operated on in the same scope.

**Example fix:**

```toke
m=demo;
t=$Vec2{x:i64;y:i64};
f=scale(a:$Vec2;factor:i64):$Vec2{
  <$Vec2{x:a.x*factor;y:a.y*factor}
};
```

Inline the expressions to avoid the shadowing locals.

**Auto-fix (`toke lint --fix`):** No. Inlining may not always be safe or readable, especially when the expression is large.

**Edge cases:**

- Only flags when the shadowing name matches a field of a struct type that appears in the same function's parameter list or a `let` binding within the same scope.
- Does not flag if the struct type has no fields with that name reachable in the current scope.
- A single-letter field name collision (e.g. `x`, `y`) is more likely to be flagged than longer names — this is by design; short generic names on structs with matching fields are the highest-risk case.
