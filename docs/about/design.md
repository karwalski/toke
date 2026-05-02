---
title: Design Principles
slug: design
section: about
order: 4
---

toke's design follows a single rule: where any implementation decision conflicts with a design principle, the principle takes precedence over convenience. These are the principles.

## Machine-First Syntax

The language defines exactly one canonical syntactic form for each construct. Synonym constructs, optional delimiters, and style-variant spellings are prohibited. There is one way to declare a function, one way to return a value, one way to define a type. The model never chooses between equivalent forms.

## The 55-Character Minimal ASCII Subset

toke uses exactly 55 structural ASCII characters. No character outside this set may appear in a structural position. Arbitrary UTF-8 is permitted inside string literal content.

| Class | Characters | Count |
|---|---|---|
| Lowercase | `a-z` | 26 |
| Digits | `0-9` | 10 |
| Symbols | `( ) { } = : . ; + - * / < > ! \| $ @ %` | 19 |

The double-quote `"` appears in source as the string literal delimiter but is not a structural symbol -- it is consumed during lexing and never produces a token, similar to how whitespace separates tokens but has no other structural role. The ampersand `&` appears as the function reference prefix (`&name`) but is consumed by the lexer as part of function reference token scanning. Underscore, `^`, and `~` are excluded; bitwise operators are deferred to v0.5+.

**What is excluded:** whitespace separates tokens but has no other structural role -- indentation, line breaks, and spacing between tokens are all equivalent (the semicolon is the universal statement terminator). toke source is comment-free by design -- comments are not part of the language. Documentation lives in companion files (`.tkc`), not inline. The lexer silently discards `(* ... *)` sequences for backward compatibility, but this is not a language feature. Uppercase letters, `_`, `^`, `~`, `[`, `]`, `#`, backtick, backslash, single-quote, comma, and question-mark do not appear in structural positions.

**Why restricted:** every character in the set must be necessary. Every token in generated output must carry semantic information. A smaller, predictable character set means fewer token boundary splits in BPE tokenizers and a tighter generation space for the model. The `$` and `@` sigils replace uppercase type names (`$user` instead of `User`) and bracket-based array syntax (`@T` instead of `[T]`), producing consistent forms that a purpose-built BPE tokenizer can absorb into single merged tokens. The current Phase 1 tokenizer already merges patterns like `64):` and `){<` as single tokens; a Phase 2 retrained tokenizer is projected to merge declaration prefixes (`m=`, `f=`, `i=`) and full type signatures (`(n:i64):i64{`) as well.

## LL(1) Grammar

The toke grammar is LL(1): the parser requires exactly one token of lookahead. No backtracking. No context-sensitive disambiguation. A valid toke source file parses to exactly one unambiguous syntax tree.

This matters for two reasons:

1. **Single-pass parsing.** The compiler processes source in one forward pass. No multi-pass resolution, no deferred disambiguation. Compilation is fast and predictable.
2. **Reduced generation space.** At every syntactic position, there is exactly one valid interpretation. The set of syntactically invalid programs the model can generate is as small as the grammar allows.

## 13 Keywords

toke reserves exactly 13 identifiers as keywords. For comparison, Python has 35 and JavaScript has 64.

| Keyword | Role | Why it exists |
|---|---|---|
| `m` | Module declaration | Every source file begins with `m=module.path;`. Identifies the compilation unit. |
| `f` | Function definition | `f=name(args):$returntype{body};` -- the only way to define a function. |
| `t` | Type definition | `t=$name{fields};` -- the only way to define a struct type. |
| `i` | Import declaration | `i=alias:module.path;` -- explicit aliased imports, no wildcards. |
| `if` | Conditional branch | `if(condition){body}` -- standard conditional. |
| `el` | Else branch | Follows the closing `}` of an `if` block. Two characters instead of four. |
| `lp` | Loop | The single loop construct. No `for`, `while`, `do`, or `foreach` variants. |
| `br` | Break | Exits the innermost `lp` block. |
| `let` | Immutable binding | `let x=42;` -- binds a value that cannot be reassigned. |
| `mut` | Mutable qualifier | `let x=mut.0;` -- marks a binding as reassignable. Reassign with `x=newvalue;` (no `let`). |
| `as` | Explicit type cast | No implicit conversions. Every cast is visible in the source. |
| `rt` | Return (long form) | Equivalent to the `<` operator. Two characters instead of six. |
| `mt` | Match expression | `mt expr{$variant:binding result; ...}` -- exhaustive pattern matching on sum types and error results. |

Single lowercase characters for declarations (`m`, `f`, `t`, `i`). Two-character lowercase for control flow and match (`if`, `el`, `lp`, `br`, `mt`). Three-character lowercase for bindings (`let`, `mut`). Every keyword is as short as it can be while remaining unambiguous.

## Explicit Everything

toke prohibits implicit behaviour at every level:

- **No implicit returns.** Every function body must contain an explicit return (`<` or `rt`).
- **No optional semicolons.** The semicolon is the universal statement terminator. It is always required.
- **No structural whitespace.** Whitespace separates tokens but has no other structural role -- indentation, line breaks, and spacing between tokens are all equivalent. Two programs that differ only in whitespace between tokens are lexically identical.
- **No implicit conversions.** Type casts use `as` and are always visible in the source.
- **No undefined behaviour.** Every operation in a well-typed program has a defined result or a defined trap.
- **No hidden state.** No context-sensitive semantic changes. No global mutable state without explicit declaration.

## Strong Explicit Typing

All values, interfaces, and function signatures have explicitly stated types. There is no type inference. The model always knows what type it is working with, and the compiler always has enough information to validate without cross-file analysis beyond declared imports.

## Structured Error Handling

Errors in toke are values, not exceptions. There is no `try`/`catch`, no `throw`, no stack unwinding.

Functions that can fail return a result-or-error type. The `!` operator propagates errors to the caller. The compiler enforces that every error path is handled explicitly. This means:

- The model never forgets a `try` block.
- Error handling is visible in the type signature.
- The compiler can verify error handling completeness statically.

## Arena Memory Discipline

Memory allocation follows a lexical arena discipline. All heap allocations within a function body or explicit arena block are freed deterministically when the scope exits. No garbage collector. No reference counting. No manual `free()` calls. Memory safety is enforced by scope rules, not by runtime overhead.

## Structured Diagnostics

The compiler emits all diagnostics as structured JSON with a stable schema. Every diagnostic includes:

- A stable error code
- A machine-parseable source location (file, line, column, span)
- A severity level
- A suggested fix, where one is mechanically derivable

This means the generate-compile-repair loop does not require the model to parse English error messages. Repair can be partially mechanical -- the compiler tells the model exactly what is wrong and, in many cases, exactly how to fix it.

## The Tradeoff

toke is more concise than Python, but not less readable. The syntax is compressed, not obfuscated. A developer can read `f=fib(n:i64):i64{if(n<2){<n;}<fib(n-1)+fib(n-2);};` and understand it -- it is a function named `fib` that takes an integer and returns an integer.

The real tradeoff is breadth. toke does not have the ecosystem of Python or the flexibility of JavaScript. It is not designed to. It is designed to do one thing exceptionally well: be the most efficient language an LLM can generate, compile, and repair.

Human readability is a requirement, not a casualty. But where human convenience conflicts with token efficiency -- verbose keywords, optional syntax, synonym constructs -- token efficiency wins.
