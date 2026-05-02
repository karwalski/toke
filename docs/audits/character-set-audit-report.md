# Toke Character Set Audit Report

**Story:** 77.1.10
**Date:** 2026-05-01
**Synthesised from:** 9 sub-audits (stories 77.1.1 through 77.1.9)
**Spec version:** toke-spec-v0.3

---

## Symbol Inventory (23 symbols)

| Symbol | Uses | Essential? | Removal Impact | Recommendation |
|--------|------|-----------|---------------|----------------|
| `(` | Function calls, parameter lists, grouping, array/map literals `@(...)`, block comments `(* *)`, conditionals, loops, string interpolation (deferred) | Yes | Total language failure — no calls, no collections, no comments | Keep |
| `)` | Closes all `(` contexts | Yes | Same as `(` | Keep |
| `{` | Function bodies, struct/type bodies, if/else/loop bodies, match arms, arena blocks | Yes | No block scoping mechanism | Keep |
| `}` | Closes all `{` contexts | Yes | Same as `{` | Keep |
| `=` | Binding (`let x=`), assignment, all declarations (`m=`, `f=`, `t=`, `i=`), equality comparison | Yes | No binding, no declarations, no equality | Keep |
| `:` | Type annotations, return types, struct field types, map key:value, import alias, match arm binding | Yes | No type system syntax | Keep |
| `.` | Member/field access, module paths, `mut.` qualifier, float decimals, `.get()` indexing | Yes | No qualified access, no floats | Keep |
| `;` | Statement terminator, param/arg separator, field separator, array/map element separator, match arm separator, loop clause separator | Yes | Ambiguous statement boundaries, LL(1) loss | Keep |
| `+` | Integer/float addition, string concatenation, array concatenation | Yes | No addition or concatenation | Keep |
| `-` | Integer/float subtraction, unary negation | Yes | No subtraction | Keep |
| `*` | Integer/float multiplication, pointer type prefix (FFI) | Yes | No multiplication | Keep |
| `/` | Integer/float division (single use, no overload) | Yes | No division | Keep |
| `<` | Return shorthand (`<expr;`), less-than comparison, left-shift (as `<<`) | Yes | No short returns, no comparison | Keep |
| `>` | Greater-than comparison, right-shift (as `>>`) | Yes | No comparison | Keep |
| `!` | Error propagation (postfix `expr!$err`), logical NOT (prefix), error type in signatures | Yes | No error handling syntax | Keep |
| `\|` | Match block opener (`expr\|{...}`), bitwise OR, logical OR (as `\|\|`), union type separator | Yes | No pattern matching | Keep |
| `$` | Type name prefix, variant tag prefix, struct literal prefix | Functional but replaceable | Legacy profile works without it (PascalCase) | Keep (default profile identity) |
| `@` | Array type prefix (`@i64`), array/map literals (`@(...)`), map types (`@(K:V)`) | Functional but replaceable | Legacy profile uses `[]` instead | Keep (default profile identity) |
| `&` | Bitwise AND (implemented), function reference prefix (spec'd, NOT implemented) | Questionable | Zero usage as bitwise AND anywhere; `&name` not yet working | Defer bitwise; implement `&name` |
| `^` | Bitwise XOR | Questionable | Zero usage anywhere | Defer |
| `~` | Bitwise NOT (unary) | Questionable | Zero usage anywhere | Defer |
| `%` | Modulo/remainder | Yes | Commonly needed arithmetic op | Keep |
| `_` | Identifier word separator (naming convention only) | No | 115 stdlib renames, ~1030 test instances; purely cosmetic | Removable (matches M0 decision) |

---

## 1. Essential Symbols (cannot be removed)

| Symbol | Justification |
|--------|---------------|
| `( )` | Function calls, grouping, collection literals, comments — no alternative exists |
| `{ }` | Sole block-scoping mechanism; LL(1) grammar depends on it |
| `=` | Only binding/assignment operator; only equality operator; all 4 declaration forms use it |
| `:` | Only type annotation syntax; key:value in maps; import aliasing |
| `.` | Only member access syntax; module paths; float literals |
| `;` | Only statement terminator; universal separator (replaces `,`) |
| `+ - * /` | Fundamental arithmetic; no keyword alternatives |
| `%` | Modulo is commonly needed and correctly placed at multiplicative precedence |
| `< >` | Comparison operators; `<` also serves as return shorthand (core design choice) |
| `!` | Error propagation is central to toke's error handling model |
| `\|` | Match syntax `expr\|{...}` has no alternative; logical OR (`\|\|`) is standard |

**Count: 17 symbols (including paired delimiters counted individually)**

---

## 2. Questionable Symbols (could potentially be removed/deferred)

### `& ^ ~ << >>` (Bitwise operators)

- **Current status:** Fully implemented in lexer, parser, type checker, and LLVM codegen
- **Usage:** Zero instances in stdlib, tests, examples, or any known toke source file
- **Impact of removal:** Nothing breaks. All implementation becomes dead code.
- **Complication:** `&` is dual-purposed (bitwise AND + future function reference prefix). Removing bitwise AND simplifies the `&name` implementation since no disambiguation is needed.

### `_` (Underscore)

- **Current status:** Allowed in identifiers (`[a-z_][a-z0-9_]*`)
- **M0 decision (frozen 2026-03-27):** Explicitly excluded. Re-added based on data that overstated embedding depth.
- **Impact of removal:** 115 stdlib identifier renames, ~1030 test/example instance renames. All mechanical (find-replace). Zero structural dependency.
- **Complication:** Story 75.1.5 (2026-04-30) decided `$snake_case` for variants — this conflicts with M0 and would need reversal.

### `$` (Dollar sigil)

- **Current status:** Required prefix for all user-defined type names and variant tags in default profile
- **Alternative:** Legacy profile uses PascalCase — language is fully functional without `$`
- **Impact of removal:** Would require case-sensitivity for type vs variable disambiguation, or a different mechanism
- **Note:** `$` is a defining characteristic of toke's default syntax and aids parser simplicity (no lookahead needed for type identification)

### `@` (At sigil)

- **Current status:** Collection type/literal prefix in default profile
- **Alternative:** Legacy profile uses `[]` — same semantics, different characters
- **Impact of removal:** Would require `[` and `]` (two characters replacing one)
- **Note:** `@` is cleaner than `[]` from a character-budget perspective

---

## 3. Minimal Viable Character Set

The absolute minimum set of symbols toke could function with:

```
( ) { } = : . ; + - * / < > ! | %
```

**17 symbols** (down from 23). This removes `$ @ & ^ ~ _` and requires:

- PascalCase for type names (replacing `$`)
- `[]` for collections (replacing `@`, but adds 2 new chars — net change: remove 1, add 2)
- No bitwise operations (removing `& ^ ~`)
- No underscore in identifiers (concatenated lowercase only)

**Practical minimum (preserving default profile identity):**

```
( ) { } = : . ; + - * / < > ! | % $ @
```

**19 symbols.** This defers `& ^ ~ _` (the zero-usage and zero-structural-need characters).

---

## 4. Key Findings for Researcher Attention

### 4.1 Underscore was excluded at M0, re-added based on misleading data

The M0 design decision (story 1.1.2, frozen 2026-03-27) explicitly removed underscore. It was later re-introduced via stdlib naming conventions. The "719 instances deeply embedded" framing overstated the difficulty — removal is purely mechanical: 115 stdlib renames, ~1030 test instance renames, 2 lines changed in lexer.c. No structural dependency exists.

### 4.2 `$` is functional but the legacy profile proves the language works without it

The `--legacy` profile compiles valid toke programs using PascalCase instead of `$`-prefixed types. The two profiles are isomorphic (`$user` = `User`). The `$` sigil provides parser simplicity and visual disambiguation but is not structurally necessary.

### 4.3 `& ^ ~ << >>` (bitwise) have zero usage anywhere

All five bitwise operators are fully implemented (lexer through LLVM codegen) but have zero instances in stdlib `.tki` files, zero test cases, zero example programs, and zero corpus usage. They are dead code that could be deferred to a future version with no impact.

### 4.4 `&` as function reference prefix is NOT YET IMPLEMENTED despite being spec'd

Section 24.8 of the spec describes `&name` as "PROMOTED (limited)" for creating first-class function values. The compiler has no implementation — no `NODE_FUNC_REF` AST node, no handling of `TK_AMP` in `parse_primary()`. Using `&name` today produces a parse error. This is a gap between spec and implementation.

### 4.5 `|` requires LL(2) peek for match disambiguation

The parser at `parse_bitor()` uses `peek_at(p, 1)` to distinguish `|{` (match block) from `| expr` (bitwise OR). This is technically LL(2) — two tokens of lookahead are needed to resolve the ambiguity. The spec acknowledges this in a grammar comment. While trivial to implement (single `peek_at` call), it technically violates a strict LL(1) claim if one is made.

---

## 5. Recommendations for Researchers

The following decisions warrant re-review given audit data:

1. **Underscore inclusion** — Should the M0 exclusion be reinstated? The data shows removal is trivial, and keeping underscore adds complexity (E1006 trailing rule, E1007 double-underscore rule, BPE tokenizer inefficiency from split boundaries).

2. **Bitwise operator set** — Should `& ^ ~ << >>` be deferred to v0.5+? They have zero usage, add 4 characters to the set, and `&` specifically complicates the function-reference feature that IS needed.

3. **LL(1) claim vs `|` disambiguation** — Is the LL(2) peek for match blocks acceptable, or should the match syntax be redesigned (e.g., a keyword like `mt` instead of `|{`)?

4. **`&name` priority** — Should function references be implemented before bitwise AND is exercised? The spec marks it "promoted" and it enables callback patterns (HTTP handlers, event loops) that are needed for real programs.

5. **Character set count** — If `& ^ ~ _` are deferred, should the spec's character count be updated to reflect the actually-exercised set (19 symbols vs 23)?

---

## Appendix: Sub-Audit Sources

| # | Story | File | Symbols covered |
|---|-------|------|-----------------|
| 1 | 77.1.1 | symbols-core-structural.md | `( ) { } ; : . =` |
| 2 | 77.1.2 | symbols-arithmetic.md | `+ - * /` |
| 3 | 77.1.3 | symbols-comparison-logic.md | `< > ! \| & && \|\|` |
| 4 | 77.1.4 | symbols-dollar-sigil.md | `$` |
| 5 | 77.1.5 | symbols-at-sigil.md | `@` |
| 6 | 77.1.6 | symbols-underscore-removal.md | `_` |
| 7 | 77.1.7 | symbols-bitwise-ops.md | `& ^ ~ % << >>` |
| 8 | 77.1.8 | symbols-string-escape.md | `"` `\` (lexer-level, not structural) |
| 9 | 77.1.9 | symbols-comments.md | `(* *)` (reuses `( * )`) |
