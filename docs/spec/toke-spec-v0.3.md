---
title: toke Language Specification
slug: toke-spec-v0.3
section: spec
order: 5
---

## Version 0.3

**Language name:** toke
**Written shorthand:** tk
**Compiler binary:** tkc
**File extension:** .tk
**Package registry name:** tokelang
**Specification status:** Draft — normative sections marked [N], informative sections marked [I]

---

## Table of Contents

1. Purpose
2. Scope
3. Design Principles
4. Goals and Non-Goals
5. Standardisation Layers
6. Terminology and Notation
7. Character Set (55 Characters)
8. Lexical Rules
9. Source Model
10. Grammar — Formal EBNF
11. Core Language Constructs
12. Type System
13. Error Model
14. Memory Model
15. Module and Interface Structure
16. Standard Library Interface
17. Compiler Requirements
18. Structured Diagnostics Specification
19. Tooling Protocol
20. Conformance
21. Benchmarking and Adoption Criteria
22. Security and Safety Requirements
23. Reference Implementation Requirements
24. Deferred Items
25. Governance and Versioning
26. Appendix A — Error Code Registry
27. Appendix B — Diagnostic Schema (normative)
28. Appendix C — Tooling Protocol Schema (normative)
29. Appendix D — Keyword and Symbol Reference
30. Appendix E — Reserved Identifiers
31. Appendix F — Legacy Profile

---

## Changelog

### v0.3.2

**New normative sections:**
- Package system (§24.3): manifest format (`pkg.toml`), `pkg.*` namespace, MVS resolution, git-based distribution, lock file (`pkg.lock`), import integration, local cache, diagnostics E9010-E9014/W9010. Design only — compiler implementation deferred to v0.6+.

### v0.3.1

**Design reversals:**
- Comments are no longer a language feature. `(* ... *)` is retained as a lexer tolerance mechanism for Phase 2 training compatibility only (§8.10). Documentation comments `(** ... *)` removed.
- Companion files (`.tkc`) promoted from reserved to first-class language mechanism (§24.11). Companion files are the designated home for documentation, API docs, and metadata. Format still reserved for v0.4.

### v0.3

**Breaking changes:**
- Function reference syntax changed from `f=name` to `&name` (§24.8)
- Error and tag variant naming standardised to `$concatenatedlowercase` — PascalCase and snake_case variants removed (§11.5, §13.2, §16.x, Appendix E)
- Character set reduced from 59 to 55 (§7.1) — removed `_`, `&` (as bitwise), `^`, `~`

**New features:**
- Comment tolerance: lexer silently discards `(* ... *)` sequences for Phase 2 compatibility (§8.10)
- Modulo operator `%` (§11.15)
- Match expression keyword `mt` — replaces `expr|{...}` syntax (§11.12)
- Function references via `&name` promoted to implemented (§24.8)
- `let` shadowing: same-scope re-declaration creates new binding (§11.8.x)
- Normative LL(1) grammar appendix with FIRST/FOLLOW sets (Appendix X)

**Promoted from deferred:**
- Tokenizer vocabulary: promoted to normative (§24.7)
- Function references via `&name`: promoted to "implemented (limited)" (§24.8)

**Newly deferred with milestones:**
- Bitwise operators (`&` as bitwise, `^`, `~`, `<<`, `>>`): v0.5+
- Option type: v0.4
- Closures with environment capture: v0.4
- Concurrency: v0.5+
- Package registry: v0.6+

**Promoted from reserved:**
- `.tkc` companion files promoted to first-class language mechanism (§24.11) — format still reserved for v0.4

**Reserved:**
- Multimodal LLM code generation contract (minimal statement, formal protocol reserved)

### v0.2
- Default syntax (55-char profile) adopted as primary
- Legacy 80-char profile retained as `--legacy`

### v0.1
- Initial draft specification

---

## 1. Purpose [N]

toke is a statically typed, machine-native programming language and compilation workflow designed primarily for reliable generation, repair, validation, and execution by large language models and other code-generating systems.

The purpose of toke is to define a language and associated tooling standard that:

- minimises token overhead in source representation
- eliminates syntactic ambiguity and multiple equivalent forms for the same construct
- supports deterministic parsing and type checking from the first character of any construct
- provides machine-readable, structured compiler diagnostics suitable for automated repair loops
- supports iterative generate-compile-inspect-repair workflows without human interpretation of error output
- compiles to efficient native binaries through a stable backend with no runtime dependency
- enables future interoperability through typed intermediate representations and protocol-level integration

toke is not initially optimised for human ergonomic expressiveness. Its primary optimisation target is machine generation reliability and token efficiency. Human readability is a secondary property, not a design constraint.

---

## 2. Scope [N]

This specification defines:

- the normative properties of the toke source language (Layer A)
- the structure of source files and modules
- the complete character set (55 chars)
- the formal grammar in EBNF
- the type system baseline
- the error model and memory model
- compiler behaviour and required compilation phases
- the structured diagnostic format
- the tooling protocol interface
- conformance requirements and test categories
- benchmarking criteria for adoption as a broader standard

This specification version 0.1 does not fully specify:

- concurrency semantics (Section 24.1)
- full foreign function interface rules (Section 24.2 — experimental normative text now present)
- package registry governance (Section 24.3)
- formal memory model with ownership annotations (Section 24.4)
- debugger metadata formats (Section 24.5)
- canonical binary intermediate representation (Section 24.6)
- tokenizer vocabulary (Section 24.7)

Those are listed as deferred items in Section 24.

---

## 3. Design Principles [N]

All toke implementations shall adhere to the following design principles. Where any implementation decision conflicts with a principle, the principle takes precedence over convenience.

### 3.1 Machine-first syntax

The language shall define exactly one canonical syntactic form for each construct. Synonym constructs, optional delimiters, and style-variant spellings are prohibited.

### 3.2 Deterministic structure

A valid toke source unit shall parse to exactly one unambiguous syntax tree under the normative grammar. The grammar is LL(1): the parser shall never require more than one token of lookahead to determine the applicable production.

### 3.3 Token efficiency

The source language shall minimise unnecessary verbosity, redundant delimiters, whitespace-as-syntax, and optional surface variation. Every character in the character set must be necessary. Every token in the generated output must carry semantic information.

### 3.4 Strong explicit typing

All values, interfaces, and function signatures shall have explicitly stated types. Implicit type inference is not defined in version 0.1. Every type boundary is visible in source.

### 3.5 Structured failure

All compiler and runtime outputs shall be machine-readable and schema-stable. Free-form prose error messages shall not be the primary diagnostic channel. Every error condition shall have a stable error code, a machine-parseable location, and a suggested fix where the fix is mechanically derivable.

### 3.6 Incremental compilability

A single source file shall be independently parseable and type-checkable given only its declared imports and the type interfaces those imports expose. No source file shall require knowledge of a caller's context to compile.

### 3.7 No hidden behaviour

The language shall not define implicit conversions, context-sensitive semantic changes, runtime magic, or undefined behaviour in well-typed programs. Every operation has a defined result or a defined trap behaviour.

### 3.8 Native execution target

The standard shall support native compilation producing self-contained binaries for x86-64 and ARM64. No runtime interpreter, virtual machine, or garbage collector dependency shall be required in the distributed binary.

### 3.9 Arena memory discipline

Memory allocation shall follow a lexical arena discipline: all heap allocations within a function body or explicit arena block are freed deterministically on scope exit. Manual heap allocation and manual freeing are not exposed as first-class operations in version 0.1.

---

## 4. Goals and Non-Goals [I]

### 4.1 Goals

toke aims to:

- improve LLM code generation first-attempt success rates
- reduce source token count by a measurable margin under both existing and purpose-trained tokenizers
- support multi-file typed programs where each file contains one primary export
- enable stable automated repair loops through structured compiler feedback
- support reproducible benchmarking and conformance testing across model families
- compile to native binaries with high runtime performance
- support future typed IR and protocol standardisation as Layer C and Layer D extensions

### 4.2 Non-Goals

toke does not aim to:

- replace general-purpose languages for all human-written software
- optimise for manual developer ergonomics or interactive development environments
- provide unrestricted metaprogramming, macros, or compile-time code execution
- support multiple equivalent coding styles or optional syntax variants
- standardise a full package ecosystem in version 0.1
- define a universal latent or binary inter-model protocol in version 0.1
- support garbage collection, reference counting, or any form of automatic memory management other than arena allocation

---

## 5. Standardisation Layers [N]

The toke standard is divided into four layers. Each layer depends on the layer below it.

### Layer A — Source Language

Normative syntax, lexical rules, type system, file layout, and semantics. Fully specified in version 0.1.

### Layer B — Compiler Contract

Structured diagnostics schema, exit behaviour, interface extraction format, and artefact generation. Fully specified in version 0.1.

### Layer C — Tooling Protocol

Standard request and response shapes for compilation, validation, testing, and repair workflows. Partially specified in version 0.1; Section 19 defines the normative baseline.

### Layer D — Intermediate Representation

A future typed IR or canonical AST serialisation for machine-to-machine exchange. Deferred to version 0.2 (Section 24.6).

**Version 0.1 standardises Layers A and B in full, and provides a normative baseline for Layer C.**

---

## 6. Terminology and Notation [I]

The key words **shall**, **shall not**, **should**, **should not**, **may**, and **may not** are used throughout this specification with the meanings defined in RFC 2119.

- **shall** — normative requirement; a conforming implementation must satisfy this
- **shall not** — normative prohibition
- **should** — recommended behaviour; deviation requires documented justification
- **may** — permitted but not required

Additional terms:

| Term | Definition |
|------|------------|
| source file | a UTF-8 encoded text file with extension `.tk` |
| module | a named collection of source files sharing a namespace |
| construct | a syntactic unit recognised by the grammar |
| token | a single atomic lexical unit produced by the lexer |
| tk token | an LLM vocabulary token in the tokenizer sense (distinguished from lexical token by context) |
| legacy profile | the 80-character variant of the language using uppercase keywords and `[]` arrays; available via `--legacy` flag (see Appendix F) |
| tkc | the reference compiler binary |
| arena | a lexically scoped memory region whose allocations are freed on scope exit |
| diagnostic | a structured machine-readable compiler message |
| repair loop | an automated cycle of generate, compile, extract diagnostic, fix, recompile |

---

## 7. Character Set (55 Characters) [N]

toke source uses exactly 55 structural characters drawn from a minimal printable-ASCII subset, lowercase-only, with no uppercase letters and no exotic punctuation. No character outside this set shall appear in toke source except within string literal content, where arbitrary UTF-8 is permitted.

### 7.1 Complete Character Table

```
CLASS        CHARACTERS                                                 COUNT
──────────────────────────────────────────────────────────────────────────────
Lowercase    a b c d e f g h i j k l m n o p q r s t u v w x y z       26
Digits       0 1 2 3 4 5 6 7 8 9                                       10
Symbols      ( ) { } = : . ; + - * / < > ! | $ @ %                     19
──────────────────────────────────────────────────────────────────────────────
TOTAL                                                                   55
```

Note: The double-quote `"` appears in source as the string delimiter for string literals but is not a structural symbol — it is consumed by the lexer during string literal scanning and never produces a token. It is analogous to whitespace in this regard.

Note: The ampersand `&` appears in source as the function reference prefix (`&name`) but is not counted as a structural symbol — it is consumed by the lexer as part of function reference token scanning and produces a `TK_AMP` token only in that context. Bitwise use of `&` is deferred to v0.5+.

### 7.2 Excluded Characters

The following character classes are explicitly excluded from structural toke source:

- Uppercase letters (A-Z) — not used in structural positions; type references use the `$` sigil instead
- Square brackets `[` `]` — not used; array literals use `@(...)`, array types use `@$type`
- Whitespace (space U+0020, tab U+0009, carriage return U+000D, line feed U+000A) — structurally meaningless; permitted only within string literals
- Comment sequences `(* ... *)` — tolerated and discarded by the lexer (Section 8.10); not a language feature
- `_` (underscore) — was excluded at M0, reinstated in error, now re-excluded
- `^`, `~` — bitwise operators deferred to v0.5+
- `#`, `` ` ``, `\`, `'`, `,`, `?` — not assigned

The absence of whitespace as a structural element means the lexer produces a flat token stream with no position-dependent meaning. Two programs that differ only in whitespace between tokens are lexically identical.

### 7.3 String Literal Content

String literals are delimited by `"`. Within a string literal, any valid UTF-8 sequence is permitted. The following escape sequences are defined:

| Sequence | Meaning |
|----------|---------|
| `\"` | literal double-quote |
| `\\` | literal backslash |
| `\n` | newline (U+000A) |
| `\t` | horizontal tab (U+0009) |
| `\r` | carriage return (U+000D) |
| `\0` | null byte (U+0000) |
| `\xNN` | byte with hexadecimal value NN |

No other escape sequences are defined. An unrecognised escape sequence is a compile error (E1001).

### 7.4 Multi-Character Operators

The following multi-character sequences are recognised as single tokens:

| Sequence | Token | Meaning |
|----------|-------|---------|
| `&&` | TK_AND | logical and |
| `\|\|` | TK_OR | logical or |

### 7.5 Tokenizer Efficiency [I]

The purpose-built toke BPE tokenizer is trained on the default-syntax corpus and targets the following single-token merges:

**Declaration prefixes (appear in every program):** `m=`, `f=`, `i=`, `t=`

**Type signatures (high frequency):** `:i64`, `:str`, `:bool`, `:i64):i64{`, `():i64{`

**Stdlib imports (always followed by `;`):** `std.io;`, `std.str;`, `std.json;`, `std.http;`

**Control flow and return patterns:** `if(`, `el{`, `<0};`, `){<`

**Common compound patterns:** `io.println(`, `main():i64{`, `.len`, `.get(`

The Phase 1 tokenizer (8K vocab, trained on legacy corpus) achieves 13.1% token reduction vs cl100k_base. The Phase 2 retrained tokenizer on default syntax is estimated to achieve 40--75% fewer tokens than equivalent Python, C, or Java programs -- verified against a complete fibonacci benchmark program (~23 tokens vs 41--102 for other languages). The tokenizer is being finalised as part of Gate 2.

---

## 8. Lexical Rules [N]

### 8.1 Token Classes

The lexer produces tokens of the following classes:

| Class | Description | Examples |
|-------|-------------|---------|
| IDENT | User-defined identifier or context keyword | `getuser` `count` `m` `f` `t` `i` |
| TYPE_IDENT | `$`-prefixed type reference | `$user` `$str` `$i64` |
| INT_LIT | Integer literal | `0` `42` `1024` `0xFF` |
| FLOAT_LIT | Floating-point literal | `3.14` `0.5` `1.0e9` |
| STR_LIT | String literal | `"hello"` `"user \(id)"` |
| BOOL_LIT | Boolean literal | `true` `false` |
| LPAREN | `(` | |
| RPAREN | `)` | |
| LBRACE | `{` | |
| RBRACE | `}` | |
| EQ | `=` | |
| COLON | `:` | |
| DOT | `.` | |
| SEMI | `;` | |
| PLUS | `+` | |
| MINUS | `-` | |
| STAR | `*` | |
| SLASH | `/` | |
| LT | `<` | |
| GT | `>` | |
| BANG | `!` | |
| PIPE | `\|` | |
| DOLLAR | `$` | |
| AT | `@` | |
| AND | `&&` | |
| OR | `\|\|` | |
| AMP | `&` | |
| PERCENT | `%` | |
| EOF | End of input | |

### 8.2 Context Keywords

The declaration keywords `m`, `f`, `t`, `i` are **not** reserved words. The lexer emits them as `TK_IDENT`. The parser recognises them as declaration introducers only when they appear at the top level followed by `=` (i.e., `m=`, `f=`, `t=`, `i=`). Inside function bodies, these identifiers may be used as variable names.

The following identifiers are reserved keywords:

| Keyword | Role |
|---------|------|
| `if` | conditional branch |
| `el` | else branch (follows `if` block only) |
| `lp` | loop (the single loop construct) |
| `br` | break — exits the innermost loop |
| `let` | immutable binding |
| `mut` | mutable qualifier on binding |
| `as` | type cast |
| `rt` | return (long-form alternative to `<` for clarity in nested expressions) |
| `mt` | match expression |

Boolean literals `true` and `false` are not keywords; they are predefined identifiers. They may not be redefined.

### 8.3 Lexical Precedence

When multiple token classes could match at the current position, the following precedence applies:

1. KEYWORD — reserved identifiers take precedence over IDENT
2. IDENT — lowercase-initial identifiers
3. Numeric literals — longest match
4. String literals — delimited by `"`, terminated by unescaped `"`
5. Multi-character operators (`&&`, `||`) — longest match
6. Single-character symbols — exact match

### 8.4 Identifier Rules

An identifier matches the pattern `[a-z][a-z0-9]*` with the following constraints:

- begins with a lowercase letter (a-z)
- continues with lowercase letters and digits (0-9) only
- must not collide with a reserved keyword
- is case-sensitive

Type references are prefixed with `$` and are lowercase: `$user`, `$str`, `$i64`.

### 8.5 Integer Literals

Integer literals may be:
- decimal: one or more digits `0-9`
- hexadecimal: `0x` followed by one or more digits `0-9`, `a-f`, `A-F`
- binary: `0b` followed by one or more digits `0` or `1`

Integer literals have no suffix. The type of an integer literal is inferred from its binding context. If context does not resolve the type, the literal is typed as `i64` by default.

### 8.6 Float Literals

Float literals consist of:
- a decimal integer part
- a `.` separator
- a decimal fractional part
- an optional exponent: `e` or `E`, optional sign, one or more digits

Float literals have no suffix. The default float type is `f64`.

### 8.7 String Literals

String literals begin and end with `"`. Content may include any UTF-8 codepoint subject to the escape sequences defined in Section 7.3. String interpolation uses `\(expr)` syntax where `expr` is a toke expression that must resolve to a `$str`-compatible type.

### 8.8 Lexer Errors

The lexer shall emit a structured diagnostic (Section 18) and halt on:
- an invalid escape sequence within a string literal (E1001)
- an unterminated string literal at end of file (E1002)
- a character outside the character set in a structural position (E1003)
- an identifier beginning with a digit (E1004)

### 8.9 Whitespace and Token Separation [N]

#### 8.9.1 Whitespace Characters

The following characters are whitespace in toke:

| Character | Unicode | Name |
|-----------|---------|------|
| ` ` | U+0020 | Space |
| `\t` | U+0009 | Horizontal tab |
| `\r` | U+000D | Carriage return |
| `\n` | U+000A | Line feed |

Whitespace is not a structural element of toke source. It carries no syntactic meaning beyond its role as a token separator. The lexer discards all whitespace outside string literals after using it to determine token boundaries.

#### 8.9.2 When Whitespace Is Required

Whitespace is **required** between two adjacent tokens when both tokens consist entirely of identifier characters (letters `a`–`z`, digits `0`–`9`). Without whitespace, the lexer applies longest-match and produces a single token, not two.

**Normative rule:** If token A ends with an identifier character and token B begins with an identifier character, at least one whitespace character shall appear between them. Omitting this whitespace does not produce a syntax error at the token boundary; instead, A and B merge into a single token under longest-match, which is then classified as an identifier or rejected by the parser as an unexpected token.

#### 8.9.3 When Whitespace Is Not Required

Whitespace is **not required** at a token boundary where at least one adjacent token consists of a symbol character (operators, brackets, punctuation). The lexer can unambiguously determine the boundary without whitespace.

Examples where whitespace is optional:

| Written | Tokens produced | Equivalent to |
|---------|-----------------|---------------|
| `x+y` | `x` `+` `y` | `x + y` |
| `a=b` | `a` `=` `b` | `a = b` |
| `f(x)` | `f` `(` `x` `)` | `f ( x )` |
| `a:i64` | `a` `:` `i64` | `a : i64` |
| `<x` | `<` `x` | `< x` |

#### 8.9.4 Whitespace Equivalence

Any amount of whitespace between two tokens is equivalent. A single space, multiple spaces, a tab, a newline, or any combination of whitespace characters between two tokens produces the same token stream. Two programs that differ only in the whitespace between tokens are semantically identical.

**Normative rule:** The compiler shall treat any non-empty sequence of whitespace characters as equivalent to a single space for all purposes of parsing and semantic analysis.

#### 8.9.5 Longest-Match Lexing and Keyword Prefixes

The lexer uses longest-match (maximal munch): at each position it consumes the longest sequence of characters that forms a valid token. This rule applies uniformly and has the following consequence for keyword recognition:

A token that begins with a keyword string but continues with additional identifier characters is classified as an **identifier**, not as a keyword followed by additional characters. The lexer does not split an identifier-character run at keyword boundaries.

**Normative rule:** An identifier whose spelling begins with a reserved keyword prefix is a valid identifier, not an error. The keyword is recognised only when it appears as a complete token — that is, when it is followed by a non-identifier character or end of input.

#### 8.9.6 Annotated Examples

| Source text | Tokens produced | Classification |
|-------------|-----------------|----------------|
| `letx` | `letx` | identifier `letx` |
| `let x` | `let` `x` | keyword `let` + identifier `x` |
| `letmut` | `letmut` | identifier `letmut` |
| `let mut` | `let` `mut` | keyword `let` + keyword `mut` |
| `mutantninjaturtles` | `mutantninjaturtles` | identifier `mutantninjaturtles` |
| `mut antninjaturtles` | `mut` `antninjaturtles` | keyword `mut` + identifier `antninjaturtles` |
| `letmutantninjaturtles=5` | `letmutantninjaturtles` `=` `5` | identifier assigned, not a let-binding |
| `let mutantninjaturtles=5` | `let` `mutantninjaturtles` `=` `5` | let-binding of identifier `mutantninjaturtles` |

The rule is mechanical and context-free: the lexer does not consult surrounding syntax or parser state when deciding token boundaries. Longest-match is applied unconditionally.

### 8.10 Comment Tolerance [N]

The lexer recognises `(* ... *)` sequences and silently discards them. No tokens are produced. Nesting is permitted: `(* outer (* inner *) *)` is a single discarded sequence.

Comments are **not** part of the toke language design. Source files are expected to be comment-free. The `(* ... *)` tolerance exists solely as a compatibility mechanism for Phase 2 training data that may contain comment sequences.

Documentation for toke programs lives in companion files (see Section 24.11).

---

## 9. Source Model [N]

### 9.1 Source File Structure

A toke source file (.tk) consists of a sequence of declarations in the following mandatory order:

1. Module declaration (exactly one, required)
2. Import declarations (zero or more, all before any other declarations)
3. Type declarations (zero or more)
4. Constant declarations (zero or more)
5. Function declarations (zero or more)

No other ordering is valid. A declaration of kind N shall not appear before all declarations of kind N-1 are complete.

The compiler shall emit diagnostic E2001 if declarations appear out of order.

### 9.2 One Primary Export Per File

Each source file should export exactly one primary construct. This is a strong convention enforced by the project linter, not the compiler. Files exporting multiple primary constructs are valid but produce a linter warning (L0001).

This discipline ensures that LLM generation context for a single file is bounded to:
- the types of its imports (interface files, ~40 tokens each)
- the function signature being implemented (~20 tokens)
- the task description

A typical toke generation context is under 500 LLM tokens.

### 9.3 File Encoding

Source files shall be encoded as UTF-8. A byte order mark (BOM) at the start of a file shall be silently ignored. Any non-UTF-8 byte sequence is a compiler error (E1005).

---

## 10. Grammar — Formal EBNF [N]

The following EBNF defines the complete normative grammar for toke. Terminals are shown in single quotes or as token class names in UPPERCASE. Nonterminals are shown in PascalCase. `?` means zero or one. `*` means zero or more. `+` means one or more. `|` is alternation. `()` is grouping.

```ebnf
(* Top-level structure *)
SourceFile      = ModuleDecl ImportDecl* TypeDecl* ConstDecl* FuncDecl* EOF ;

(* Module — 'm' is a context keyword, not a reserved word *)
ModuleDecl      = 'm' '=' ModulePath ';' ;
ModulePath      = IDENT ( '.' IDENT )* ;

(* Imports — 'i' is a context keyword *)
ImportDecl      = 'i' '=' IDENT ':' ModulePath ';' ;

(* Type declarations — 't' is a context keyword *)
TypeDecl        = 't' '=' '$' TypeName '{' FieldList '}' ';' ;
TypeName        = IDENT ;
FieldList       = Field ( ';' Field )* ;
Field           = IDENT ':' TypeExpr ;

(* Constant declarations *)
ConstDecl       = IDENT '=' LiteralExpr ':' TypeExpr ';' ;

(* Function declarations — 'f' is a context keyword *)
FuncDecl        = 'f' '=' IDENT '(' ParamList ')' ':' ReturnSpec '{' StmtList '}' ';' ;
ParamList       = Param ( ';' Param )* | (* empty *) ;
Param           = IDENT ':' TypeExpr ;
ReturnSpec      = TypeExpr ( '!' TypeExpr )? ;

(* Statements *)
StmtList        = Stmt* ;
Stmt            = BindStmt
                | MutBindStmt
                | AssignStmt
                | ReturnStmt
                | IfStmt
                | LoopStmt
                | BreakStmt
                | ExprStmt
                | ArenaStmt
                ;

BindStmt        = 'let' IDENT '=' Expr ';' ;
MutBindStmt     = 'let' IDENT '=' 'mut' '.' Expr ';' ;
AssignStmt      = IDENT '=' Expr ';' ;
ReturnStmt      = '<' Expr ';' | 'rt' Expr ';' ;
BreakStmt       = 'br' ';' ;

IfStmt          = 'if' '(' Expr ')' '{' StmtList '}' ( 'el' '{' StmtList '}' )? ;
LoopStmt        = 'lp' '(' Stmt Expr ';' Stmt ')' '{' StmtList '}' ;
ArenaStmt       = '{' 'arena' StmtList '}' ;

ExprStmt        = Expr ';' ;

(* Expressions — precedence low to high *)
Expr            = LogOrExpr ;
LogOrExpr       = LogAndExpr ( '||' LogAndExpr )* ;
LogAndExpr      = EqExpr ( '&&' EqExpr )* ;
EqExpr          = CompareExpr ( '=' CompareExpr )? ;
CompareExpr     = AddExpr ( ( '<' | '>' ) AddExpr )? ;
AddExpr         = MulExpr ( ( '+' | '-' ) MulExpr )* ;
MulExpr         = UnaryExpr ( ( '*' | '/' | '%' ) UnaryExpr )* ;
UnaryExpr       = '-' UnaryExpr
                | '!' UnaryExpr
                | MatchExpr
                ;
MatchExpr       = 'mt' PropagateExpr '{' MatchArmList '}'
                | PropagateExpr
                ;
PropagateExpr   = CallExpr ( '!' TypeExpr )? ;
CallExpr        = PostfixExpr ( '(' ArgList ')' )* ;
PostfixExpr     = PrimaryExpr ( '.' IDENT )* ;
PrimaryExpr     = IDENT
                | LiteralExpr
                | '(' Expr ')'
                | StructLit
                | ArrayLit
                | MapLit
                | FuncRef
                ;
FuncRef         = '&' IDENT ;

(* Match arms *)
MatchArmList    = MatchArm ( ';' MatchArm )* ;
MatchArm        = TypeExpr ':' IDENT Expr ;

(* Struct literal — $name{field:val; ...} *)
StructLit       = '$' IDENT '{' FieldInit ( ';' FieldInit )* '}' ;
FieldInit       = IDENT ':' Expr ;

(* Array literal — @(expr; expr; ...) *)
ArrayLit        = '@' '(' ( Expr ( ';' Expr )* )? ')' ;

(* Map literal — @(key:val; key:val; ...) *)
MapLit          = '@' '(' Expr ':' Expr ( ';' Expr ':' Expr )* ')' ;

(* Argument list *)
ArgList         = Expr ( ';' Expr )* | (* empty *) ;

(* Type expressions *)
TypeExpr        = ScalarType
                | '$' IDENT
                | ArrayTypeExpr
                | MapTypeExpr
                | FuncTypeExpr
                ;
ScalarType      = 'u8'  | 'u16' | 'u32' | 'u64'
                | 'i8'  | 'i16' | 'i32' | 'i64'
                | 'f32' | 'f64'
                | 'bool'
                | 'str'
                | 'byte'
                ;
ArrayTypeExpr   = '@' TypeExpr ;
MapTypeExpr     = '@' '(' TypeExpr ':' TypeExpr ')' ;
FuncTypeExpr    = '(' TypeExpr ( ';' TypeExpr )* ')' ':' TypeExpr ;

(* Literals *)
LiteralExpr     = INT_LIT | FLOAT_LIT | STR_LIT | BOOL_LIT ;
```

### 10.1 Grammar Properties [N]

The grammar as defined is:
- **Context-free** — no production requires semantic context to resolve
- **LL(1)** — every production is unambiguously determined by the next token
- **Unambiguous** — no input string has more than one parse tree under this grammar

Any implementation that requires more than one token of lookahead is non-conforming.

### 10.2 Grammar Validation [I]

The reference ANTLR4 grammar, generated from the EBNF above, shall be part of the conformance suite and shall produce no ambiguity warnings. Parser generators that report shift-reduce or reduce-reduce conflicts on this grammar indicate an implementation error, not a specification ambiguity.

---

## 11. Core Language Constructs [N]

### 11.1 Declaration Keywords

The four declaration keywords are context-sensitive identifiers recognised by the parser when followed by `=` at the top level:

| Keyword | Role | Notes |
|---------|------|-------|
| `m=` | module declaration | context keyword; `m` is a valid variable name inside functions |
| `f=` | function definition | context keyword; `f` is a valid variable name inside functions |
| `t=` | type definition | context keyword; `t` is a valid variable name inside functions |
| `i=` | import declaration | context keyword; `i` is a valid variable name inside functions |

These are not reserved words. The lexer emits `TK_IDENT` for `m`, `f`, `t`, `i`; the parser checks context (top-level position followed by `=`) to distinguish declarations from variable references.

### 11.2 Reserved Keywords

The following 9 identifiers are reserved and may not be used as user-defined identifiers:

| Keyword | Role |
|---------|------|
| `if` | conditional branch |
| `el` | else branch (follows `if` block only) |
| `lp` | loop (the single loop construct) |
| `br` | break — exits the innermost loop |
| `let` | immutable binding |
| `mut` | mutable qualifier on binding |
| `as` | type cast |
| `rt` | return (long-form alternative to `<` for clarity in nested expressions) |
| `mt` | match expression |

Boolean literals `true` and `false` are not keywords; they are predefined identifiers. They may not be redefined.

### 11.3 Module Declaration

Every source file begins with exactly one module declaration:

```
m=module.path;
```

The module path is a dot-separated sequence of lowercase identifiers. It identifies the module globally. Two source files with the same module path are part of the same module unless they conflict on exported names, which is a compiler error (E2005).

**Example:**
```
m=api.user;
```

### 11.4 Import Declarations

```
i=localalias:module.path;
```

`localalias` is the name by which the imported module's exports are accessed within this file. `module.path` is the fully qualified module path.

All imports must precede all type, constant, and function declarations. Wildcard imports are prohibited (E2006).

**Example:**
```
i=http:std.http;
i=db:std.db;
i=json:std.json;
```

After this, `http.$req`, `db.one`, `json.enc` etc. are accessible.

### 11.5 Type Declarations

```
t=$typename{field1:$type1;field2:$type2};
```

A type declaration defines either a **struct type** or a **sum type** (tagged union). The distinction is lexical:
- Struct: all field names are plain lowercase identifiers
- Sum type: all field names use the `$` sigil prefix (variant tags)
- Mixing is a compile error (E2011)

**Struct example:**
```
t=$user{id:u64;name:str;email:str};
```

**Sum type (error variants) example:**
```
t=$usererr{
  $notfound:u64;
  $badinput:str;
  $dberr:str
};
```

In a sum type, each field name is a `$`-prefixed variant tag and its type is the payload. The zero-payload variant is expressed with type `bool` and the value `true` is implicit.

Tag and error variant names use the `$` prefix followed by concatenated lowercase letters and digits. Underscores and uppercase letters are not permitted in variant names.

### 11.6 Function Declarations

```
f=name(param1:$type1;param2:$type2):$returntype!$errortype{
  body
};
```

Every function declaration shall explicitly state:
- all parameter names and their types
- the return type
- the error type if the function is fallible (marked with `!$errortype`)

A function without `!$errortype` is total: it shall not contain any `!` error-propagation operations that could fail (E3001).

A function with `!$errortype` is partial: all error-propagation operations within the body must be of a type coercible to `$errortype`.

**Example — total function:**
```
f=add(a:i64;b:i64):i64{
  <a+b;
};
```

**Example — partial function:**
```
f=getuser(id:u64):$user!$usererr{
  r=db.one("SELECT id,name FROM users WHERE id=?";@(id))!$usererr.$dberr;
  <$user{id:r.u64(id);name:r.str(name)};
};
```

### 11.7 Return Statement

Two syntactically equivalent return forms are defined:

```
<expr        short form — preferred in single-expression bodies
rt expr      long form — preferred when the return is deeply nested
```

Both forms require the expression to match the declared return type. A missing return in a non-void function is a compile error (E3005).

### 11.8 Bindings

**Immutable binding:**
```
let name=expr;
```
After binding, `name` cannot be reassigned. Attempting reassignment is a compile error (E3010).

**Mutable binding:**
```
let name=mut.initial_value;
```
The `mut.` qualifier marks the binding as mutable. The initial value is required. Mutable bindings may be reassigned with `name=new_value;`.

**Bare assignment (to existing mutable binding):**
```
name=new_value;
```
Assigning to an immutable binding is a compile error (E3010). Assigning to an undeclared name is a compile error (E3011).

### 11.8.1 Let Shadowing [N]

A `let` or `let mut` binding may shadow a prior binding of the same name in the same scope or an enclosing scope:

```
let x = 1;
let x = x + 1;  (* shadows the first x *)
```

Shadowing creates a new binding; the prior binding is unreachable from after the shadowing point. The new binding may have a different type from the prior binding.

Function declarations (`f=`), type declarations (`t=`), and import declarations (`i=`) may NOT shadow existing declarations of the same name in the same scope — doing so is error E3012.

### 11.9 Conditionals

```
if(condition){
  body
}
```

```
if(condition){
  true_body
}el{
  false_body
}
```

The condition must be of type `bool`. No implicit truthiness conversion is performed (E4001 for non-bool condition).

`el` is not a standalone keyword; it is only valid immediately after a closing `}` of an `if` block.

Nested `if` chains:
```
if(a){
  body_a
}el{
  if(b){
    body_b
  }el{
    body_c
  }
}
```

### 11.10 Loop

toke defines exactly one loop construct:

```
lp(init_stmt;condition;step_stmt){
  body
}
```

- `init_stmt`: a binding or assignment executed once before the loop
- `condition`: a `bool` expression evaluated before each iteration
- `step_stmt`: a statement executed after each iteration body

`br` exits the innermost loop immediately.

There is no `while`, `do-while`, `for-each`, or `until` construct. Recursive functions serve as the idiomatic alternative for functional patterns.

**Example — sum array:**
```
f=sum(arr:@i64):i64{
  let acc=mut.0;
  lp(let i=0;i<arr.len;i=i+1){
    acc=acc+arr.get(i)
  };
  <acc
};
```

### 11.11 Logical Operators

toke provides two logical operators:

| Operator | Meaning |
|----------|---------|
| `&&` | logical and (short-circuiting) |
| `\|\|` | logical or (short-circuiting) |

Both operands must be of type `bool`. These operators are lower precedence than comparison operators.

**Example:**
```
if(a>0 && b>0){
  <a+b
};
```

### 11.12 Match Expression

```
mt expr {
  $variant1:binding1 result_expr;
  $variant2:binding2 result_expr
}
```

The `mt` keyword introduces a match expression. The expression following `mt` is evaluated and matched against the arms in the block. Each arm names a `$`-prefixed variant, binds its payload to a local name, and provides a result expression.

Match is **exhaustive**: the compiler rejects any match that does not cover all variants (E4010). When a variant is added to a sum type, all match expressions on that type fail to compile until they are updated.

The match expression is an expression, not a statement. Its result type is the common type of all arm expressions; all arms must return the same type (E4011).

**Example:**
```
mt getuser(id) {
  $ok:u   <$res.ok(json.enc(u));
  $err:e  <$res.err(json.enc(e))
}
```

### 11.13 Error Propagation

```
expr!$errorvariant
```

The `!` operator propagates an error from a partial call. If `expr` evaluates to `$err(e)`, the current function returns `$err($errorvariant(e))`. If `expr` evaluates to `$ok(v)`, execution continues with value `v`.

The right-hand side of `!` must be a variant constructor of the current function's declared error type (E3020).

**Example:**
```
f=handle(req:$http.req):$http.res!$apierr{
  body=json.dec(req.body)!$apierr.$badrequest;
  user=db.getuser(body.id)!$apierr.$dberror;
  <$http.res.ok(json.enc(user))
};
```

### 11.14 Arena Blocks

```
{arena
  stmts
}
```

An arena block creates a lexically scoped allocation region. All heap allocations made within the block are freed when the block exits, regardless of how it exits (normal completion, early return, or error propagation). The arena block is an explicit tool for controlling allocation lifetime within a function body.

Returning a pointer or reference to an arena-allocated value across the arena boundary is a type error (E5001).

### 11.15 Modulo Operator [N]

| Operator | Name | LLVM IR | Operand types |
|----------|------|---------|---------------|
| `%` | Modulo (remainder) | `srem` | Integer |

The `%` operator sits at the multiplicative precedence tier alongside `*` and `/`.

**Operator Precedence** (highest to lowest):
1. Unary: `-`, `!`
2. Multiplicative: `*`, `/`, `%`
3. Additive: `+`, `-`
4. Comparison: `<`, `>`
5. Equality: `=`
6. Logical AND: `&&`
7. Logical OR: `||`

The `|` symbol is used in type declarations as a union type separator. Bitwise operators (`&`, `^`, `~`, `<<`, `>>`) are deferred to v0.5+.

---

## 12. Type System [N]

### 12.1 Primitive Types

| Type | Description | Width |
|------|-------------|-------|
| `u8` | unsigned integer | 8-bit |
| `u16` | unsigned integer | 16-bit |
| `u32` | unsigned integer | 32-bit |
| `u64` | unsigned integer | 64-bit |
| `i8` | signed integer | 8-bit |
| `i16` | signed integer | 16-bit |
| `i32` | signed integer | 32-bit |
| `i64` | signed integer | 64-bit |
| `f32` | IEEE 754 binary32 | 32-bit |
| `f64` | IEEE 754 binary64 | 64-bit |
| `bool` | boolean | 1 logical bit |
| `str` | UTF-8 string | heap-allocated |
| `byte` | single byte (alias for `u8`) | 8-bit unsigned |

In type expressions, primitive types are written without the `$` sigil: `i64`, `str`, `bool`. User-defined types require `$`: `$user`, `$mytype`.

### 12.2 Array Types

`@$t` is a dynamically-sized array of elements of type `$t`. Arrays are heap-allocated within the current arena. The `.len` member returns a `u64`. Array access uses `arr.get(i)` which returns type `$t`; out-of-bounds access at runtime is a trap (Section 14.2).

**Array literal:**
```
let a = @(1; 2; 3);
```

**Array type in signature:**
```
f=sum(arr:@i64):i64{ ... };
```

### 12.3 Map Types

`@($k:$v)` is a map from key type `$k` to value type `$v`.

**Map literal:**
```
let m = @(1:10; 2:20; 3:30);
```

### 12.4 Record Types

Declared with `t=$name{...}`. Fields are accessed with `.fieldname`. Records are value types — assignment copies the record. Records may not contain pointers to themselves (no recursive struct definitions without indirection — E2015).

**Struct literal:**
```
let p = $point{x: 1; y: 2};
```

### 12.5 Sum Types

Declared with `t=$name{$variant1:$payloadtype;...}`. Sum types are the mechanism for error types and tagged unions. The `$result` type is a built-in sum type:

```
t=$result{$ok:$t;$err:$e}
```

All partial functions return `$result` implicitly, expressed in the signature as `:$t!$e`.

### 12.6 No Implicit Coercion

No implicit type coercions are defined. Specifically prohibited:

- numeric widening (e.g. `i32` to `i64` automatically) — use `as`
- string conversion (integer to string, etc.) — use stdlib `str.fromint` etc.
- truthiness conversion (integer or pointer to bool) — use explicit comparison

The `as` keyword performs explicit type cast:
```
let wide = narrow as i64;
```

Casts that could truncate or lose precision generate a compile-time warning (W1001) and a runtime trap if the value does not fit.

### 12.7 Type Checking Rules

The type checker shall enforce:

1. Every identifier reference is resolved to exactly one declaration (E3011 for unresolved, E3012 for ambiguous)
2. Every function call argument matches the declared parameter type exactly (E4020)
3. Every return expression matches the declared return type (E4021)
4. Every error propagation `!` variant is a variant of the function's declared error type (E3020)
5. Match arms are exhaustive and cover exactly the variants of the matched type (E4010)
6. All match arms return the same type (E4011)
7. Arena-allocated values do not escape the arena (E5001)
8. Mutable bindings are not captured in closures (closures are deferred — Section 24.8)
9. Struct field access refers to a field declared in that struct (E4025)
10. Array access index is of type `u64` or a type promotable to `u64` via `as` (E4026)

### 12.8 Generics (Deferred)

Generic type parameters are deferred to version 0.2. The collection types `@$t` are built-in and do not require a generic annotation in source. All user-defined types are concrete in version 0.1.

---

## 13. Error Model [N]

### 13.1 The Result Type

All fallible operations return a `$result` type. The declaration syntax `:$t!$e` desugars to a return of `$result`. The `!` propagation operator is the primary mechanism for working with results.

### 13.2 Error Type Declaration

Error types are sum types where each variant represents a distinct failure mode:

```
t=$dberr{
  $connectionfailed:str;
  $queryfailed:str;
  $notfound:u64;
  $timeout:u32
};
```

### 13.3 Error Propagation Rules

The `!` operator: `expr!$targetvariant`

1. Evaluates `expr`
2. If `$ok(v)`: returns `v`, execution continues
3. If `$err(e)`: constructs `$targetvariant(e)` and returns `$err($targetvariant(e))` from the current function

The type of `e` must be coercible to the payload type of `$targetvariant`. The type of `$targetvariant` must be a variant of the current function's declared error type.

If the error types are not coercible, the compiler emits E3021.

### 13.4 Explicit Match on Errors

When propagation is not appropriate, errors are matched explicitly:

```
mt result {
  $ok:v  handlesuccess(v);
  $err:e handleerror(e)
}
```

### 13.5 Runtime Traps

Certain conditions cannot be detected statically and produce runtime traps:

| Condition | Trap code |
|-----------|-----------|
| Array out-of-bounds access | RT001 |
| Integer division by zero | RT002 |
| Integer overflow (checked arithmetic) | RT003 |
| Null pointer dereference (FFI only) | RT004 |
| Arena boundary violation | RT005 |
| Stack overflow | RT006 |
| Assertion failure | RT007 |

Runtime traps write a structured trap record to stderr and terminate the process with exit code 2. The trap record format is defined in Appendix B.

### 13.6 No Exceptions

toke does not define exception semantics, stack unwinding, or catch blocks. All error handling is explicit at every call site. This is a design constraint, not a limitation.

---

## 14. Memory Model [N]

### 14.1 Arena Allocation

toke uses lexical arena allocation. Every function body is an implicit arena. All allocations made within the function body (strings, arrays, structs) are freed when the function returns, via any path.

Explicit `{arena ... }` blocks create sub-arenas with shorter lifetimes. These are freed when the block exits.

The allocator is a bump allocator per arena, providing O(1) allocation cost. Deallocation is bulk: the entire arena is freed in a single operation on scope exit.

### 14.2 Allocation Rules

1. Allocations within a function body are valid for the lifetime of that function call
2. Allocations within an arena block are valid for the lifetime of that block
3. A value allocated in inner scope A shall not be returned from, passed out of, or stored in a location with a lifetime longer than A (E5001)
4. Module-level constants and static data are allocated at program start and freed at program exit

### 14.3 Static Lifetime

Module-level declarations (constants, connection pools, caches) have static lifetime. They are initialised once at program start in declaration order and are never freed during execution. Circular initialisation dependencies are a compile error (E2020).

### 14.4 No Pointer Arithmetic

Pointer arithmetic is not defined in version 0.1. Raw pointers are only accessible through FFI declarations (deferred, Section 24.2). Within toke source, all memory access is through named fields, array `.get()` calls, and function calls.

### 14.5 Memory Safety Guarantees

In well-typed toke code (no FFI):
- No use-after-free: arena discipline prevents access to freed memory
- No buffer overflow: array bounds are checked at runtime (RT001)
- No uninitialised reads: all bindings require initialisation at declaration
- No null pointer dereference: toke has no null values (Option type is deferred to 0.2 — use sum types)
- No data races: concurrency is deferred to 0.2

### 14.6 Extended Memory Model Specification

The full formal memory model — including lifetime rules, concurrency memory semantics, FFI boundary rules, and formal safety proofs — is specified in the [Memory Model Specification](/docs/spec/memory-model/).

---

## 15. Module and Interface Structure [N]

### 15.1 Module Identity

A module is identified by its fully-qualified path (e.g. `api.user`, `std.http`). The path determines the expected file location relative to the project root:

```
api/user.tk       -> module api.user
std/http.tk       -> module std.http
```

### 15.2 Import Resolution

At the start of type checking, the compiler resolves all imports. For each import:

1. Locate the source file for the declared module path
2. Load its exported interface
3. Make the exported symbols available under the declared local alias

If a module path cannot be resolved, the compiler emits E2030 and lists the modules available in the current project.

### 15.3 Circular Imports

Circular imports are prohibited (E2031). The import graph must be a directed acyclic graph. The compiler detects cycles before type checking begins.

### 15.4 Exports

All top-level declarations in a source file are exported by default. In version 0.1 there is no access control modifier. A future version may introduce explicit visibility annotations.

### 15.5 Interface Files

The compiler shall emit a `.tki` interface file for each compiled module. The interface file contains:

```
module: str
version: str
exports: [
  { name: str; kind: "type"|"func"|"const"; signature: str }
]
```

Interface files are consumed during import resolution. They allow incremental compilation: a module can be type-checked against its dependencies' interface files without recompiling the dependencies from source.

### 15.6 Wildcard Imports

Wildcard imports are prohibited in version 0.1. Every imported symbol must be accessed through its alias. This ensures that the source of every identifier is unambiguous without reading import declarations.

---

## 16. Standard Library Interface [N]

The standard library modules are part of the toke specification. Their signatures are normative; their implementations are not. There are 49 modules grouped into nine categories.

### Core

#### 16.1 std.str [I]

String manipulation and conversion.

**Types:** `$SliceErr{msg:str}` `$ParseErr{msg:str}` `$EncodingErr{msg:str}` `$BracketErr{msg:str}` `$StrBuf{}` (opaque builder)

**Functions:**

- `str.len(s:str):u64` — byte length
- `str.concat(a:str;b:str):str` — concatenate two strings
- `str.slice(s:str;start:u64;end:u64):str!SliceErr` — substring by byte offsets
- `str.from_int(n:i64):str` — integer to string
- `str.from_float(n:f64):str` — float to string
- `str.to_int(s:str):i64!ParseErr` — parse integer from string
- `str.to_float(s:str):f64!ParseErr` — parse float from string
- `str.contains(s:str;sub:str):bool` — substring test
- `str.split(s:str;sep:str):[str]` — split by separator
- `str.trim(s:str):str` — strip leading/trailing whitespace
- `str.upper(s:str):str` — uppercase
- `str.lower(s:str):str` — lowercase
- `str.bytes(s:str):[byte]` — UTF-8 bytes
- `str.from_bytes(b:[byte]):str!EncodingErr` — bytes to string
- `str.startswith(s:str;pfx:str):bool` — prefix test
- `str.endswith(s:str;sfx:str):bool` — suffix test
- `str.replace(s:str;old:str;new:str):str` — replace all occurrences
- `str.indexof(s:str;sub:str):i64` — first index of substring, -1 if absent
- `str.repeat(s:str;n:u64):str` — repeat n times
- `str.join(sep:str;parts:[str]):str` — join with separator
- `str.buf():StrBuf` — create string builder
- `str.add(b:StrBuf;s:str):void` — append string to builder
- `str.addbyte(b:StrBuf;c:byte):void` — append byte to builder
- `str.done(b:StrBuf):str` — finalise builder to string
- `str.trimprefix(s:str;pfx:str):str` — strip prefix
- `str.trimsuffix(s:str;sfx:str):str` — strip suffix
- `str.lastindex(s:str;sub:str):i64` — last index of substring
- `str.matchbracket(s:str):str!BracketErr` — extract bracket-matched content

#### 16.2 std.encoding [I]

Base64, hex, and URL encoding/decoding.

**Types:** `$EncodingErr{msg:str}`

**Functions:**

- `encoding.b64encode(data:[byte]):str` — base64 encode
- `encoding.b64urlencode(data:[byte]):str` — URL-safe base64 encode
- `encoding.b64decode(s:str):[byte]!EncodingErr` — base64 decode
- `encoding.b64urldecode(s:str):[byte]!EncodingErr` — URL-safe base64 decode
- `encoding.hexencode(data:[byte]):str` — hex encode
- `encoding.hexdecode(s:str):[byte]!EncodingErr` — hex decode
- `encoding.urlencode(s:str):str` — percent-encode
- `encoding.urldecode(s:str):str!EncodingErr` — percent-decode

#### 16.3 std.json [I]

JSON parsing, extraction, streaming, and writing.

**Types:** `$Json{raw:str}` `$JsonErr{$Parse:str;$Type:str;$Missing:str}` `$JsonWriter{buf:[byte];pos:u64}` `$JsonStream` (opaque) `$JsonToken{$ObjectStart;$ObjectEnd;$ArrayStart;$ArrayEnd;$Key:str;$Str:str;$U64:u64;$I64:i64;$F64:f64;$Bool:bool;$Null;$End}` `$JsonStreamErr{$Truncated:str;$Invalid:str;$Overflow:str}`

**Functions:**

- `json.enc(v:str):str` — encode value to JSON string
- `json.dec(s:str):Json!JsonErr` — decode JSON string to document
- `json.str(j:Json;key:str):str!JsonErr` — extract string field
- `json.u64(j:Json;key:str):u64!JsonErr` — extract u64 field
- `json.i64(j:Json;key:str):i64!JsonErr` — extract i64 field
- `json.f64(j:Json;key:str):f64!JsonErr` — extract f64 field
- `json.bool(j:Json;key:str):bool!JsonErr` — extract bool field
- `json.arr(j:Json;key:str):[Json]!JsonErr` — extract array field
- `json.streamparser(data:[byte]):JsonStream` — create streaming parser
- `json.streamnext(s:JsonStream):JsonToken!JsonStreamErr` — next token from stream
- `json.newwriter(cap:u64):JsonWriter` — create JSON writer with capacity
- `json.streamemit(w:JsonWriter;j:Json):void!JsonStreamErr` — emit JSON to writer
- `json.writerbytes(w:JsonWriter):[byte]` — extract bytes from writer

#### 16.4 std.toon [I]

TOON (Toke Object Notation) serialisation — the default toke data format.

**Types:** `$Toon{raw:str}` `$ToonErr{Parse:str;Type:str;Missing:str}`

**Functions:**

- `toon.enc(v:str):str` — encode to TOON
- `toon.dec(s:str):Toon!ToonErr` — decode TOON string
- `toon.str(t:Toon;key:str):str!ToonErr` — extract string field
- `toon.i64(t:Toon;key:str):i64!ToonErr` — extract i64 field
- `toon.f64(t:Toon;key:str):f64!ToonErr` — extract f64 field
- `toon.bool(t:Toon;key:str):bool!ToonErr` — extract bool field
- `toon.arr(t:Toon;key:str):[Toon]!ToonErr` — extract array field
- `toon.from_json(s:str):str` — convert JSON string to TOON
- `toon.to_json(s:str):str` — convert TOON string to JSON

#### 16.5 std.yaml [I]

YAML parsing and extraction.

**Types:** `$Yaml{raw:str}` `$YamlErr{Parse:str;Type:str;Missing:str}`

**Functions:**

- `yaml.enc(v:str):str` — encode to YAML
- `yaml.dec(s:str):Yaml!YamlErr` — decode YAML string
- `yaml.str(y:Yaml;key:str):str!YamlErr` — extract string field
- `yaml.i64(y:Yaml;key:str):i64!YamlErr` — extract i64 field
- `yaml.f64(y:Yaml;key:str):f64!YamlErr` — extract f64 field
- `yaml.bool(y:Yaml;key:str):bool!YamlErr` — extract bool field
- `yaml.arr(y:Yaml;key:str):[Yaml]!YamlErr` — extract array field
- `yaml.from_json(s:str):str` — convert JSON to YAML
- `yaml.to_json(s:str):str` — convert YAML to JSON

#### 16.6 std.toml [I]

TOML parsing and extraction.

**Types:** `$tomlval` (opaque) `$tomlerr{msg:str}`

**Functions:**

- `toml.load(s:str):tomlval!tomlerr` — parse TOML string
- `toml.loadfile(path:str):tomlval!tomlerr` — parse TOML from file
- `toml.str(v:tomlval;key:str):str!tomlerr` — extract string
- `toml.i64(v:tomlval;key:str):i64!tomlerr` — extract integer
- `toml.bool(v:tomlval;key:str):bool!tomlerr` — extract boolean
- `toml.section(v:tomlval;key:str):tomlval!tomlerr` — extract sub-table

#### 16.7 std.i18n [I]

Internationalisation — externalised string bundles.

**Types:** `$I18nBundle{data:str}` `$I18nErr{NotFound:str;Parse:str}`

**Functions:**

- `i18n.load(dir:str;locale:str):I18nBundle!I18nErr` — load string bundle for locale
- `i18n.get(b:I18nBundle;key:str):str` — look up string by key
- `i18n.fmt(b:I18nBundle;key:str;arg:str):str` — look up and interpolate
- `i18n.locale():str` — detect system locale

### I/O

#### 16.8 std.file [I]

File system read/write operations.

**Types:** `$FileErr{$NotFound:str;$Permission:str;$IO:str}`

**Functions:**

- `file.read(path:str):str!FileErr` — read entire file to string
- `file.write(path:str;content:str):bool!FileErr` — write string to file
- `file.append(path:str;content:str):bool!FileErr` — append string to file
- `file.exists(path:str):bool` — test file existence
- `file.delete(path:str):bool!FileErr` — delete file
- `file.list(dir:str):[str]!FileErr` — list directory entries
- `file.isdir(path:str):bool` — test if path is directory
- `file.mkdir(path:str):bool!FileErr` — create directory
- `file.copy(src:str;dst:str):bool!FileErr` — copy file
- `file.listall(dir:str):[str]!FileErr` — list directory recursively

#### 16.9 std.path [I]

Path manipulation (no I/O).

**Functions:**

- `path.join(a:str;b:str):str` — join path components
- `path.ext(p:str):str` — file extension including dot
- `path.stem(p:str):str` — filename without extension
- `path.dir(p:str):str` — parent directory
- `path.base(p:str):str` — filename with extension
- `path.isabs(p:str):bool` — test if absolute path

#### 16.10 std.env [I]

Environment variable access.

**Types:** `$EnvErr{$NotFound;$Invalid}`

**Functions:**

- `env.get(key:str):str!EnvErr` — get env var, error if missing
- `env.get_or(key:str;default:str):str` — get env var with default
- `env.getint(key:str;default:i64):i64` — get env var as integer with default
- `env.set(key:str;val:str):bool` — set env var

#### 16.11 std.args [I]

Command-line argument access.

**Types:** `$ArgsErr{msg:str}`

**Functions:**

- `args.all():[str]` — all arguments as array
- `args.get(i:u64):str!ArgsErr` — argument by index
- `args.count():u64` — argument count

#### 16.12 std.sys [I]

System directory paths.

**Functions:**

- `sys.configdir(app:str):str` — XDG/platform config directory for app
- `sys.datadir(app:str):str` — XDG/platform data directory for app

#### 16.13 std.process [I]

Subprocess management.

**Types:** `$Handle{}` (opaque) `$ProcessErr{$NotFound;$Permission;$IO}`

**Functions:**

- `process.spawn(argv:[str]):Handle!ProcessErr` — spawn subprocess
- `process.wait(h:Handle):i32!ProcessErr` — wait for exit, return exit code
- `process.stdout(h:Handle):str!ProcessErr` — read stdout
- `process.stderr(h:Handle):str!ProcessErr` — read stderr
- `process.kill(h:Handle):bool` — kill subprocess

### Network

#### 16.14 std.http [I]

HTTP server routes, client requests, streaming, and TLS.

**Types:** `$Req{method:str;path:str;headers:[[str]];body:str;params:[[str]]}` `$Res{status:u16;headers:[[str]];body:str}` `$HttpErr{$BadRequest:str;$NotFound:str;$Internal:str;$Timeout:u32}` `$httpclient{baseurl:str;pool_size:u64;timeout_ms:u64}` `$httpreq{method:str;url:str;headers:[[str]];body:[byte]}` `$httpresp{status:u64;headers:[[str]];body:[byte]}` `$httperr{msg:str;code:u64}` `$httpstream{id:u64;open:bool}`

**Server functions:**

- `http.param(req:Req;name:str):str!HttpErr` — extract route parameter
- `http.header(req:Req;name:str):str!HttpErr` — extract request header
- `http.Res.ok(body:str):Res` — 200 response
- `http.Res.json(status:u16;body:str):Res` — JSON response with status
- `http.Res.bad(msg:str):Res` — 400 response
- `http.Res.err(msg:str):Res` — 500 response
- `http.GET(pattern:str;handler:f)` — register GET route (compiler-expanded)
- `http.POST(pattern:str;handler:f)` — register POST route
- `http.PUT(pattern:str;handler:f)` — register PUT route
- `http.DELETE(pattern:str;handler:f)` — register DELETE route
- `http.PATCH(pattern:str;handler:f)` — register PATCH route
- `http.serve_workers(port:u64;workers:u64):void` — start pre-fork worker pool
- `http.serve_tls(port:u64;cert:str;key:str):void` — start HTTPS server
- `http.getstaticmime(path:str;body:str;mime:str):i64` — register static route with MIME type
- `http.setcors(origins:str):i64` — set CORS origins for all responses

**Client functions:**

- `http.client(baseurl:str):httpclient` — create HTTP client
- `http.withproxy(c:httpclient;proxy:str):httpclient` — set proxy
- `http.get(c:httpclient;url:str):httpresp!httperr` — GET request
- `http.post(c:httpclient;url:str;body:[byte];ct:str):httpresp!httperr` — POST request
- `http.put(c:httpclient;url:str;body:[byte];ct:str):httpresp!httperr` — PUT request
- `http.delete(c:httpclient;url:str):httpresp!httperr` — DELETE request
- `http.stream(c:httpclient;req:httpreq):httpstream!httperr` — open streaming request
- `http.streamnext(s:httpstream):[byte]!httperr` — read next chunk from stream
- `http.downloadfile(c:httpclient;url:str;dest:str):i64` — streaming file download

#### 16.15 std.ws [I]

WebSocket client connections.

**Types:** `$wsconn{id:u64;ready:bool}` `$wsmsg{payload:[byte];fin:bool;opcode:u64}` `$wserr{msg:str}`

**Functions:**

- `ws.connect(url:str):wsconn!wserr` — open WebSocket connection
- `ws.send(c:wsconn;msg:str):void!wserr` — send text message
- `ws.sendbytes(c:wsconn;data:[byte]):void!wserr` — send binary message
- `ws.recv(c:wsconn):wsmsg!wserr` — receive next message
- `ws.close(c:wsconn):void` — close connection
- `ws.broadcast(conns:[wsconn];msg:str):void` — broadcast to all connections

#### 16.16 std.sse [I]

Server-Sent Events emission.

**Types:** `$ssectx{id:u64;open:bool}` `$sseevent{id:str;event:str;data:str;retry:u64}` `$sseerr{msg:str}`

**Functions:**

- `sse.emit(ctx:ssectx;evt:sseevent):void!sseerr` — emit structured event
- `sse.emitdata(ctx:ssectx;data:str):void!sseerr` — emit data-only event
- `sse.close(ctx:ssectx):void` — close SSE stream
- `sse.keepalive(ctx:ssectx;interval_ms:u64):void` — start keepalive pings

#### 16.17 std.net [I]

Low-level network utilities.

**Functions:**

- `net.portavailable(port:int):int` — test if TCP port is available (returns 0/1)

#### 16.18 std.router [I]

Programmatic HTTP router with middleware.

**Types:** `$router` (opaque) `$ctx{req:http.Req;params:[[str]];state:[[str]]}` `$handler` (opaque) `$middleware` (opaque) `$routererr{msg:str}`

**Functions:**

- `router.new():router` — create new router
- `router.get(r:router;pattern:str;h:handler):void` — register GET route
- `router.post(r:router;pattern:str;h:handler):void` — register POST route
- `router.put(r:router;pattern:str;h:handler):void` — register PUT route
- `router.delete(r:router;pattern:str;h:handler):void` — register DELETE route
- `router.use(r:router;mw:middleware):void` — add middleware
- `router.serve(r:router;addr:str;port:u64):void!routererr` — start serving

#### 16.19 std.tls [I]

TLS connections, certificate generation, and mutual authentication.

**Types:** `$TlsConfig{cert_pem:str;key_pem:str;peer_cert_pem:str;require_mutual:bool}` `$TlsConn{id:str}` `$TlsKeypair{cert_pem:str;key_pem:str}` `$TlsErr{$CertErr:str;$ConnErr:str;$PinErr:str;$IoErr:str}`

**Functions:**

- `tls.gen_self_signed(cn:str;days:i32):TlsKeypair!TlsErr` — generate self-signed cert
- `tls.listen(port:i32;cfg:TlsConfig;handler:fn):bool` — TLS server listener
- `tls.connect(host:str;port:i32;cfg:TlsConfig):?(TlsConn)` — TLS client connect
- `tls.read(c:TlsConn):?(str)` — read from TLS connection
- `tls.write(c:TlsConn;data:str):bool` — write to TLS connection
- `tls.close(c:TlsConn):bool` — close TLS connection
- `tls.peer_cert(c:TlsConn):?(str)` — get peer certificate PEM
- `tls.fingerprint(pem:str):str` — SHA-256 fingerprint of certificate
- `tls.pairing_code(c:TlsConn):str` — derive human-readable pairing code

#### 16.20 std.mdns [I]

mDNS/DNS-SD service discovery on local network.

**Types:** `$servicerecord{name:str;type:str;port:i32;txt:@(str)}` `$discovered{name:str;host:str;port:i32;txt:@(str)}` `$MdnsErr{$NotAvailable:str;$AlreadyAdvertising:str;$NotFound:str;$Timeout:str;$Internal:str}`

**Functions:**

- `mdns.advertise(svc:servicerecord):bool` — advertise service
- `mdns.stop_advertise(name:str):bool` — stop advertising
- `mdns.browse(type:str;cb:fn(discovered):void):bool` — browse for services
- `mdns.stop_browse(type:str):bool` — stop browsing
- `mdns.resolve(name:str;type:str):?(discovered)` — resolve a specific service
- `mdns.is_available():bool` — check mDNS availability

### Security

#### 16.21 std.crypto [I]

Cryptographic hashing, HMAC, and random bytes.

**Functions:**

- `crypto.sha256(data:[byte]):[byte]` — SHA-256 hash
- `crypto.sha512(data:[byte]):[byte]` — SHA-512 hash
- `crypto.hmacsha256(key:[byte];data:[byte]):[byte]` — HMAC-SHA256
- `crypto.hmacsha512(key:[byte];data:[byte]):[byte]` — HMAC-SHA512
- `crypto.constanteq(a:[byte];b:[byte]):bool` — constant-time comparison
- `crypto.randombytes(n:u64):[byte]` — cryptographically secure random bytes
- `crypto.to_hex(data:[byte]):str` — bytes to hex string
- `crypto.sha256file(path:str):str` — SHA-256 of file contents
- `crypto.sha256verify(path:str;expected:str):bool` — verify file hash

#### 16.22 std.encrypt [I]

Symmetric/asymmetric encryption, key exchange, and signatures.

**Types:** `$DecryptResult{ok:[byte];err:str}` `$Keypair{pubkey:[byte];privkey:[byte]}`

**Functions:**

- `encrypt.aes256gcm_encrypt(key:[byte];nonce:[byte];plaintext:[byte];aad:[byte]):[byte]` — AES-256-GCM encrypt
- `encrypt.aes256gcm_decrypt(key:[byte];nonce:[byte];ciphertext:[byte];aad:[byte]):DecryptResult` — AES-256-GCM decrypt
- `encrypt.aes256gcm_keygen():[byte]` — generate AES-256 key
- `encrypt.aes256gcm_noncegen():[byte]` — generate nonce
- `encrypt.x25519_keypair():Keypair` — X25519 key pair
- `encrypt.x25519_dh(privkey:[byte];pubkey:[byte]):[byte]` — Diffie-Hellman shared secret
- `encrypt.ed25519_keypair():Keypair` — Ed25519 signing key pair
- `encrypt.ed25519_sign(privkey:[byte];msg:[byte]):[byte]` — Ed25519 sign
- `encrypt.ed25519_verify(pubkey:[byte];msg:[byte];sig:[byte]):bool` — Ed25519 verify
- `encrypt.hkdf_sha256(ikm:[byte];salt:[byte];info:[byte];len:u64):[byte]` — HKDF key derivation
- `encrypt.tls_cert_fingerprint(pem:str):[byte]` — certificate fingerprint

#### 16.23 std.auth [I]

JWT tokens and API key management.

**Types:** `$JwtAlg{$Hs256;$Hs384;$Rs256}` `$JwtClaims{sub:str;iss:str;exp:u64;iat:u64;extra:[[str]]}` `$AuthErr{msg:str;code:u64}` `$Keystore{handle:u64}`

**Functions:**

- `auth.jwtsign(claims:JwtClaims;secret:[byte];alg:JwtAlg):str!AuthErr` — sign JWT
- `auth.jwtverify(token:str;secret:[byte]):JwtClaims!AuthErr` — verify and decode JWT
- `auth.jwtexpired(claims:JwtClaims):bool` — check if JWT is expired
- `auth.apikeygenerate(prefix:str):str` — generate API key
- `auth.apikeyvalidate(key:str;store:Keystore):bool!AuthErr` — validate API key
- `auth.bearerextract(header:str):str!AuthErr` — extract Bearer token from header

#### 16.24 std.keychain [I]

OS keychain/credential store access.

**Functions:**

- `keychain.set(service:str;account:str;password:str):bool` — store credential
- `keychain.get(service:str;account:str):?(str)` — retrieve credential
- `keychain.delete(service:str;account:str):bool` — delete credential
- `keychain.exists(service:str;account:str):bool` — test if credential exists
- `keychain.is_available():bool` — check keychain availability

#### 16.25 std.secure_mem [I]

Secure memory allocation with auto-wipe.

**Types:** `$SecureBuf{id:str;size:i32;expires_at:i64}`

**Functions:**

- `secure_mem.alloc(size:i32;ttl_sec:i32):SecureBuf` — allocate secure buffer with TTL
- `secure_mem.write(buf:SecureBuf;data:str):bool` — write to secure buffer
- `secure_mem.read(buf:SecureBuf):?(str)` — read from secure buffer
- `secure_mem.wipe(buf:SecureBuf):bool` — explicitly wipe buffer
- `secure_mem.sweep():i32` — wipe all expired buffers, return count
- `secure_mem.is_available():bool` — check platform support

### Data

#### 16.26 std.db [I]

SQL database queries with parameterised statements.

**Types:** `$Row{cols:[[str]]}` `$DbErr{$Connection:str;$Query:str;$NotFound:str;$Constraint:str}`

**Functions:**

- `db.one(sql:str;params:[str]):Row!DbErr` — query single row
- `db.many(sql:str;params:[str]):[Row]!DbErr` — query multiple rows
- `db.exec(sql:str;params:[str]):u64!DbErr` — execute statement, return affected rows
- `row.str(r:Row;col:str):str!DbErr` — extract string column
- `row.u64(r:Row;col:str):u64!DbErr` — extract u64 column
- `row.i64(r:Row;col:str):i64!DbErr` — extract i64 column
- `row.f64(r:Row;col:str):f64!DbErr` — extract f64 column
- `row.bool(r:Row;col:str):bool!DbErr` — extract bool column

#### 16.27 std.csv [I]

CSV reading and writing.

**Types:** `$csvrow{fields:[str]}` `$csvreader` (opaque) `$csvwriter` (opaque) `$csverr{msg:str;line:u64}`

**Functions:**

- `csv.reader(data:[byte];sep:u8):csvreader` — create reader with separator
- `csv.next(r:csvreader):csvrow!csverr` — read next row
- `csv.header(r:csvreader):[str]!csverr` — read header row
- `csv.writer(sep:u8):csvwriter` — create writer with separator
- `csv.writerow(w:csvwriter;fields:[str]):void` — write a row
- `csv.flush(w:csvwriter):[byte]` — flush writer to bytes
- `csv.parse(data:[byte]):[csvrow]!csverr` — parse entire CSV at once

#### 16.28 std.dataframe [I]

Tabular data manipulation (imports std.csv).

**Types:** `$dataframe` (opaque; ncols, nrows) `$series{name:str;dtype:str;len:u64}` `$dfshape{rows:u64;cols:u64}` `$dferr{msg:str}`

**Functions:**

- `df.fromcsv(data:[byte]):dataframe!dferr` — parse CSV into dataframe
- `df.fromrows(cols:[str];rows:[csvrow]):dataframe` — build from column names and rows
- `df.column(d:dataframe;name:str):[f64]!dferr` — extract numeric column
- `df.columnstr(d:dataframe;name:str):[str]!dferr` — extract string column
- `df.filter(d:dataframe;col:str;op:str;val:str):dataframe!dferr` — filter rows
- `df.groupby(d:dataframe;col:str):[dataframe]!dferr` — group by column
- `df.join(a:dataframe;b:dataframe;key:str):dataframe!dferr` — inner join on key
- `df.head(d:dataframe;n:u64):dataframe` — first n rows
- `df.shape(d:dataframe):dfshape` — row and column counts
- `df.tojson(d:dataframe):[byte]!dferr` — serialise to JSON bytes
- `df.tocsv(d:dataframe):[byte]` — serialise to CSV bytes
- `df.schema(d:dataframe):[series]` — column metadata

#### 16.29 std.analytics [I]

Statistical analysis on dataframes (imports std.dataframe, std.math).

**Types:** `$statsrow{col:str;count:u64;mean:f64;stddev:f64;min:f64;p25:f64;p50:f64;p75:f64;max:f64}` `$groupstat{group:str;count:u64;sum:f64;mean:f64}` `$tspoint{ts:u64;value:f64;rolling_mean:f64}`

**Functions:**

- `analytics.describe(d:dataframe):[statsrow]!dferr` — descriptive statistics for all numeric columns
- `analytics.groupstats(d:dataframe;group_col:str;val_col:str):[groupstat]!dferr` — per-group statistics
- `analytics.timeseries(d:dataframe;ts_col:str;val_col:str;window:u64):[tspoint]!dferr` — time series with rolling mean
- `analytics.anomalies(d:dataframe;col:str;threshold:f64):[u64]!dferr` — row indices of anomalies (z-score)
- `analytics.pivot(d:dataframe;row_col:str;col_col:str;val_col:str):dataframe!dferr` — pivot table
- `analytics.corr(d:dataframe;col_a:str;col_b:str):f64!dferr` — Pearson correlation

#### 16.30 std.vecstore [I]

Embedded vector store for similarity search.

**Types:** `$VecStore{path:str}` `$VecCollection{name:str}` `$VecEntry{id:str;embedding:@(f32);payload:str;created_at:i64}` `$SearchResult{id:str;score:f64;payload:str}` `$VecErr{$IoErr:str;$DimMismatch:str;$NotFound:str}`

**Functions:**

- `vecstore.open(path:str):VecStore!VecErr` — open or create store
- `vecstore.close(s:VecStore):void` — close store
- `vecstore.collection(s:VecStore;name:str):VecCollection!VecErr` — get or create collection
- `vecstore.upsert(c:VecCollection;id:str;embedding:@(f32);dim:i32;payload:str):bool` — insert or update vector
- `vecstore.search(c:VecCollection;query:@(f32);dim:i32;k:i32;threshold:f64):[SearchResult]` — nearest-neighbour search
- `vecstore.delete(c:VecCollection;id:str):bool` — delete entry
- `vecstore.delete_before(c:VecCollection;before:i64):i32` — delete entries older than timestamp
- `vecstore.count(c:VecCollection):i32` — entry count

### AI/ML

#### 16.31 std.llm [I]

LLM client for chat, completion, and streaming (imports std.http, std.json).

**Types:** `$llmclient{provider:str;model:str}` `$llmmsg{role:str;content:str}` `$llmresp{content:str;tokens_in:u64;tokens_out:u64;model:str}` `$llmerr{msg:str;code:u64}` `$llmstream` (opaque)

**Functions:**

- `llm.client(provider:str;model:str;apikey:str):llmclient` — create LLM client
- `llm.chat(c:llmclient;msgs:[llmmsg]):llmresp!llmerr` — multi-turn chat
- `llm.chatstream(c:llmclient;msgs:[llmmsg]):llmstream!llmerr` — streaming chat
- `llm.streamnext(s:llmstream):str!llmerr` — next chunk from stream
- `llm.complete(c:llmclient;prompt:str):str!llmerr` — single-prompt completion
- `llm.countokens(c:llmclient;text:str):u64` — count tokens

#### 16.32 std.llm_tool [I]

LLM tool-use / function-calling protocol (imports std.llm).

**Types:** `$tooldecl{name:str;desc:str;params:[toolparam]}` `$toolparam{name:str;type:str;desc:str;required:bool}` `$toolcall{name:str;args:[[str]];id:str}` `$toolresult{id:str;content:str;error:bool}`

**Functions:**

- `llm.withtools(c:llmclient;tools:[tooldecl]):llmclient` — attach tool declarations
- `llm.chatwithtools(c:llmclient;msgs:[llmmsg]):toolcall!llmerr` — chat expecting tool call
- `llm.submitresult(c:llmclient;msgs:[llmmsg];result:toolresult):llmresp!llmerr` — submit tool result
- `llm.parsetoolcalls(s:str):toolcall!llmerr` — parse tool call from string
- `llm.resultmsgs(results:[toolresult]):[llmmsg]` — convert tool results to messages

#### 16.33 std.ml [I]

Classical machine learning: regression, clustering, decision trees (imports std.dataframe, std.math).

**Types:** `$row{vals:[f64]}` `$linearmodel{coef:[f64];intercept:f64}` `$centroid{id:u64;center:[f64]}` `$kmeansmodel{centroids:[centroid];k:u64}` `$dtreemodel` (opaque) `$mlerr{msg:str}`

**Functions:**

- `ml.linregfit(data:[row];target:[f64]):linearmodel!mlerr` — fit linear regression
- `ml.linregpredict(m:linearmodel;features:[f64]):f64` — predict with linear model
- `ml.kmeanstrain(data:[row];k:u64;maxiter:u64):kmeansmodel!mlerr` — train k-means
- `ml.kmeansassign(m:kmeansmodel;point:[f64]):u64` — assign point to cluster
- `ml.dtreefit(data:[row];labels:[str];maxdepth:u64):dtreemodel!mlerr` — fit decision tree
- `ml.dtreepredict(m:dtreemodel;features:[f64]):str` — predict with decision tree
- `ml.knnpredict(data:[row];labels:[str];point:[f64];k:u64):str` — k-nearest-neighbour predict

#### 16.34 std.mlx [I]

Apple MLX on-device inference (Apple Silicon only).

**Types:** `$mlxmodel{id:str;path:str}` `$mlxerr{msg:str}`

**Functions:**

- `mlx.is_available():bool` — check MLX hardware availability
- `mlx.load(path:str):mlxmodel!mlxerr` — load model from path
- `mlx.unload(m:mlxmodel):bool` — unload model
- `mlx.generate(m:mlxmodel;prompt:str;max_tokens:i32):str!mlxerr` — generate text
- `mlx.embed(m:mlxmodel;text:str):@(f32)!mlxerr` — compute embedding

#### 16.35 std.infer [I]

Local GGUF model inference with streaming support.

**Types:** `$modelhandle` (opaque) `$inferopts{n_gpu_layers:i32;n_threads:i32;seed:i32}` `$streamopts{ram_ceiling_gb:f32;prefetch_layers:i32;requires_nvme:bool}` `$infererr{msg:str;code:i32}`

**Functions:**

- `infer.load(path:str;opts:inferopts):modelhandle!infererr` — load GGUF model
- `infer.unload(h:modelhandle):bool` — unload model
- `infer.generate(h:modelhandle;prompt:str;max_tokens:i32):str!infererr` — generate text
- `infer.embed(h:modelhandle;text:str):@(f32)!infererr` — compute embedding
- `infer.is_loaded(h:modelhandle):bool` — check if model is loaded
- `infer.load_streaming(path:str;opts:streamopts):modelhandle!infererr` — load with streaming/layer prefetch

### UI/Content

#### 16.36 std.html [I]

Programmatic HTML document construction.

**Types:** `$htmldoc` (opaque) `$htmlnode{tag:str;attrs:[[str]];content:str}`

**Functions:**

- `html.doc():htmldoc` — create empty document
- `html.title(d:htmldoc;t:str):void` — set document title
- `html.style(d:htmldoc;css:str):void` — add inline style block
- `html.script(d:htmldoc;js:str):void` — add inline script block
- `html.div(attrs:[[str]];children:[htmlnode]):htmlnode` — div element
- `html.p(text:str):htmlnode` — paragraph element
- `html.h1(text:str):htmlnode` — heading 1
- `html.h2(text:str):htmlnode` — heading 2
- `html.table(headers:[str];rows:[[str]]):htmlnode` — table element
- `html.append(d:htmldoc;node:htmlnode):void` — append node to body
- `html.render(d:htmldoc):str` — render to HTML string
- `html.escape(s:str):str` — HTML-escape special characters

#### 16.37 std.svg [I]

SVG document construction.

**Types:** `$svgdoc{width:f64;height:f64;viewbox:str;elements:[svgelem]}` `$svgelem` (opaque) `$svgstyle{fill:str;stroke:str;stroke_width:f64;opacity:f64;font_size:f64;font_family:str}`

**Functions:**

- `svg.doc(w:f64;h:f64):svgdoc` — create SVG document
- `svg.rect(x:f64;y:f64;w:f64;h:f64;style:svgstyle):svgelem` — rectangle
- `svg.circle(cx:f64;cy:f64;r:f64;style:svgstyle):svgelem` — circle
- `svg.line(x1:f64;y1:f64;x2:f64;y2:f64;style:svgstyle):svgelem` — line
- `svg.path(d:str;style:svgstyle):svgelem` — SVG path
- `svg.text(x:f64;y:f64;text:str;style:svgstyle):svgelem` — text element
- `svg.group(elems:[svgelem];transform:str):svgelem` — group with transform
- `svg.polyline(points:[[f64]];style:svgstyle):svgelem` — polyline
- `svg.polygon(points:[[f64]];style:svgstyle):svgelem` — polygon
- `svg.append(doc:svgdoc;elem:svgelem):svgdoc` — append element to document
- `svg.render(doc:svgdoc):str` — render to SVG string
- `svg.style(fill:str;stroke:str;stroke_width:f64):svgstyle` — create style
- `svg.arrow(x1:f64;y1:f64;x2:f64;y2:f64;style:svgstyle):svgelem` — arrow element

#### 16.38 std.canvas [I]

HTML5 Canvas 2D drawing API (generates JavaScript).

**Types:** `$canvasop` (opaque) `$canvas{id:str;width:u32;height:u32;ops:[canvasop]}`

**Functions:**

- `canvas.new(id:str;w:u32;h:u32):canvas` — create canvas
- `canvas.fill_rect(c:canvas;x:f64;y:f64;w:f64;h:f64;color:str):canvas` — filled rectangle
- `canvas.stroke_rect(c:canvas;x:f64;y:f64;w:f64;h:f64;color:str;lw:f64):canvas` — stroked rectangle
- `canvas.clear_rect(c:canvas;x:f64;y:f64;w:f64;h:f64):canvas` — clear rectangle
- `canvas.fill_text(c:canvas;text:str;x:f64;y:f64;font:str;color:str):canvas` — draw text
- `canvas.begin_path(c:canvas):canvas` — begin path
- `canvas.move_to(c:canvas;x:f64;y:f64):canvas` — move pen
- `canvas.line_to(c:canvas;x:f64;y:f64):canvas` — line to point
- `canvas.arc(c:canvas;cx:f64;cy:f64;r:f64;start:f64;end:f64):canvas` — arc
- `canvas.close_path(c:canvas):canvas` — close path
- `canvas.fill(c:canvas;color:str):canvas` — fill current path
- `canvas.stroke(c:canvas;color:str;lw:f64):canvas` — stroke current path
- `canvas.draw_image(c:canvas;src:str;x:f64;y:f64;w:f64;h:f64):canvas` — draw image
- `canvas.set_alpha(c:canvas;alpha:f64):canvas` — set global alpha
- `canvas.to_js(c:canvas):str` — render to JavaScript
- `canvas.to_html(c:canvas):str` — render to full HTML page

#### 16.39 std.chart [I]

Chart specification (outputs JSON or Vega-Lite).

**Types:** `$dataset{label:str;data:[f64];color:str}` `$chartspec{type:str;labels:[str];datasets:[dataset];title:str}` `$charterr{msg:str}`

**Functions:**

- `chart.bar(labels:[str];values:[f64];title:str):chartspec` — bar chart
- `chart.line(labels:[str];datasets:[dataset];title:str):chartspec` — line chart
- `chart.scatter(x:[f64];y:[f64];title:str):chartspec` — scatter plot
- `chart.pie(labels:[str];values:[f64];title:str):chartspec` — pie chart
- `chart.tojson(spec:chartspec):[byte]` — serialise to JSON
- `chart.tovega(spec:chartspec):[byte]` — serialise to Vega-Lite

#### 16.40 std.dashboard [I]

Live dashboard with chart widgets and WebSocket updates (imports std.chart, std.html, std.ws, std.router).

**Types:** `$dashboard` (opaque) `$dashwidget{id:str;type:str;spec:[byte]}` `$dasherr{msg:str}`

**Functions:**

- `dashboard.new(title:str;port:u64):dashboard` — create dashboard
- `dashboard.addchart(d:dashboard;id:str;spec:chartspec):dashwidget` — add chart widget
- `dashboard.addtable(d:dashboard;id:str;headers:[str]):dashwidget` — add table widget
- `dashboard.update(d:dashboard;w:dashwidget;data:[byte]):void` — push data update
- `dashboard.serve(d:dashboard):void!dasherr` — start serving dashboard

#### 16.41 std.template [I]

Mustache-style template compilation and rendering.

**Types:** `$tmpl{id:u64;src:str}` `$tmplvars{entries:@(str:str)}` `$tmplerr{msg:str;pos:u64}`

**Functions:**

- `tpl.compile(src:str):tmpl!tmplerr` — compile template with `{{varname}}` slots
- `tpl.render(t:tmpl;vars:tmplvars):str!tmplerr` — render template with bindings
- `tpl.vars(pairs:@(str:str)):tmplvars` — create variable bindings
- `tpl.html(tag:str;attrs:@(str:str);children:@str):str` — construct HTML element inline
- `tpl.escape(s:str):str` — HTML-escape string
- `tpl.renderfile(path:str;vars:tmplvars):str!tmplerr` — load file, compile, and render

#### 16.42 std.md [I]

Markdown to HTML rendering.

**Types:** `$MdErr{msg:str}`

**Functions:**

- `md.render(src:str):str` — render Markdown string to HTML
- `md.renderfile(path:str):str!MdErr` — render Markdown file to HTML

#### 16.43 std.image [I]

Image decoding, encoding, and pixel manipulation.

**Types:** `$imgbuf{width:u32;height:u32;channels:u8;data:[byte]}` `$imgfmt{$Png;$Jpeg;$Webp;$Bmp}`

**Functions:**

- `image.decode(data:[byte]):imgbuf!str` — decode image bytes
- `image.encode(img:imgbuf;fmt:imgfmt;quality:u8):[byte]!str` — encode to format
- `image.resize(img:imgbuf;w:u32;h:u32):imgbuf` — resize image
- `image.crop(img:imgbuf;x:u32;y:u32;w:u32;h:u32):imgbuf!str` — crop region
- `image.to_grayscale(img:imgbuf):imgbuf` — convert to grayscale
- `image.flip_h(img:imgbuf):imgbuf` — flip horizontal
- `image.flip_v(img:imgbuf):imgbuf` — flip vertical
- `image.pixel_at(img:imgbuf;x:u32;y:u32):[byte]!str` — read pixel RGBA
- `image.from_raw(data:[byte];w:u32;h:u32;ch:u8):imgbuf` — construct from raw bytes

#### 16.44 std.webview [I]

Native webview window for desktop UI.

**Types:** `$webviewhandle{id:str}`

**Functions:**

- `webview.open(title:str;url:str;w:i32;h:i32):webviewhandle` — open webview window
- `webview.close(wv:webviewhandle):bool` — close window
- `webview.set_title(wv:webviewhandle;title:str):bool` — update title
- `webview.on_close(wv:webviewhandle;cb:fn():void):void` — register close callback
- `webview.register_handler(wv:webviewhandle;name:str;cb:fn(str):str):bool` — register JS-callable handler
- `webview.eval_js(wv:webviewhandle;js:str):bool` — evaluate JavaScript in webview
- `webview.run_event_loop():void` — run event loop (blocks)
- `webview.is_available():bool` — check webview availability

### System

#### 16.45 std.log [I]

Structured logging.

**Functions:**

- `log.info(msg:str;fields:[[str]]):bool` — info-level log with key-value fields
- `log.warn(msg:str;fields:[[str]]):bool` — warning-level log
- `log.error(msg:str;fields:[[str]]):bool` — error-level log
- `log.openerror(path:str;maxsize:i64;maxfiles:i64;level:i64):i64` — open error log file with rotation
- `log.accessformat(fmt:str):i64` — set access log format string

#### 16.46 std.time [I]

Time, dates, durations, timezones, and calendar functions.

**Types:** `$TimeParts{year:i64;month:i64;day:i64;hour:i64;min:i64;sec:i64}` `$Duration{years:i64;months:i64;days:i64;hours:i64;minutes:i64;seconds:i64}` `$TimeErr{$ParseErr:str;$RangeErr:str}`

**Functions:**

- `time.now():u64` — current Unix timestamp (seconds)
- `time.format(ts:u64;fmt:str):str` — format timestamp
- `time.since(ts:u64):u64` — seconds since timestamp
- `time.parse(s:str;fmt:str):u64!TimeErr` — parse timestamp from string
- `time.add(ts:u64;delta:i64):u64` — add seconds to timestamp
- `time.diff(a:u64;b:u64):i64` — difference in seconds
- `time.to_parts(ts:u64):TimeParts` — decompose to components
- `time.from_parts(p:TimeParts):u64` — compose from components
- `time.weekday(ts:u64):u64` — day of week (0=Sunday)
- `time.is_leap_year(year:i64):bool` — leap year test
- `time.days_in_month(year:i64;month:i64):u64` — days in given month
- `time.with_tz(ts:u64;tz:str):str` — format with timezone
- `time.utc_offset(tz:str):i64` — UTC offset in seconds for timezone
- `time.convert(ts:u64;from_tz:str;to_tz:str):str` — convert between timezones
- `time.add_days(ts:u64;n:i64):u64` — add days
- `time.add_months(ts:u64;n:i64):u64` — add months
- `time.add_years(ts:u64;n:i64):u64` — add years
- `time.start_of_day(ts:u64):u64` — midnight of given day
- `time.start_of_month(ts:u64):u64` — first second of month
- `time.start_of_year(ts:u64):u64` — first second of year
- `time.parse_duration(s:str):Duration!TimeErr` — parse duration string (e.g. "2h30m")
- `time.format_duration(d:Duration):str` — format duration to string
- `time.duration(a:u64;b:u64):Duration` — structured difference
- `time.julian_date(ts:u64):f64` — Julian date
- `time.mars_sol(ts:u64):f64` — Mars sol from Unix timestamp
- `time.format_mars(sol:f64;fmt:str):str` — format Mars sol
- `time.light_delay(from:str;to:str;ts:u64):f64` — light-travel delay between bodies

#### 16.47 std.math [I]

Mathematics, statistics, and linear regression.

**Types:** `$linregresult{slope:f64;intercept:f64;r2:f64}`

**Functions:**

- `math.sum(vals:[f64]):f64` — sum
- `math.mean(vals:[f64]):f64` — arithmetic mean
- `math.median(vals:[f64]):f64` — median
- `math.stddev(vals:[f64]):f64` — standard deviation
- `math.variance(vals:[f64]):f64` — variance
- `math.percentile(vals:[f64];p:f64):f64` — percentile (0-100)
- `math.linreg(x:[f64];y:[f64]):linregresult` — linear regression
- `math.min(vals:[f64]):f64` — minimum
- `math.max(vals:[f64]):f64` — maximum
- `math.abs(v:f64):f64` — absolute value
- `math.sqrt(v:f64):f64` — square root
- `math.floor(v:f64):i64` — floor
- `math.ceil(v:f64):i64` — ceiling
- `math.pow(base:f64;exp:f64):f64` — exponentiation

#### 16.48 std.mem [I]

Raw memory operations (low-level, arena-compatible).

**Functions:**

- `mem.alloc(size:u64):u64` — allocate bytes, return pointer
- `mem.free(ptr:u64):void` — free allocation
- `mem.realloc(ptr:u64;size:u64):u64` — resize allocation
- `mem.copy(dst:u64;src:u64;len:u64):void` — copy bytes
- `mem.set(ptr:u64;val:u64;len:u64):void` — fill bytes
- `mem.cmp(a:u64;b:u64;len:u64):i64` — compare bytes
- `mem.load8(ptr:u64;off:u64):u64` — load byte at offset
- `mem.store8(ptr:u64;off:u64;val:u64):void` — store byte at offset

#### 16.49 std.os [I]

POSIX-like system calls (thin wrappers).

**Functions:**

- `os.open(path:str;flags:i64;mode:i64):i64` — open file descriptor
- `os.close(fd:i64):i64` — close file descriptor
- `os.read(fd:i64;buf:i64;count:i64):i64` — read bytes
- `os.write(fd:i64;buf:i64;count:i64):i64` — write bytes
- `os.lseek(fd:i64;off:i64;whence:i64):i64` — seek
- `os.stat(path:str):i64` — file status
- `os.unlink(path:str):i64` — delete file
- `os.rename(old:str;new:str):i64` — rename file
- `os.mkdir(path:str;mode:i64):i64` — create directory
- `os.rmdir(path:str):i64` — remove directory
- `os.access(path:str;mode:i64):i64` — check access
- `os.getcwd():str` — current working directory
- `os.getpid():i64` — process ID
- `os.exit(code:i64):i64` — exit process
- `os.getenv(key:str):str` — get environment variable
- `os.setenv(key:str;val:str):i64` — set environment variable
- `os.errno():i64` — last error number
- `os.strerror(n:i64):str` — error number to string
- `os.o_rdonly():i64` — O_RDONLY constant
- `os.o_wronly():i64` — O_WRONLY constant
- `os.o_rdwr():i64` — O_RDWR constant
- `os.o_creat():i64` — O_CREAT constant
- `os.o_trunc():i64` — O_TRUNC constant
- `os.o_append():i64` — O_APPEND constant
- `os.stdin_fd():i64` — stdin file descriptor
- `os.stdout_fd():i64` — stdout file descriptor
- `os.stderr_fd():i64` — stderr file descriptor

### Testing

#### 16.50 std.test [I]

Test assertions (used with `tkc --test`).

**Functions:**

- `test.assert(cond:bool;msg:str):bool` — assert condition is true
- `test.assert_eq(actual:str;expected:str;msg:str):bool` — assert equality
- `test.assert_ne(actual:str;expected:str;msg:str):bool` — assert inequality

---

## 17. Compiler Requirements [N]

### 17.1 Required Phases

A conforming tkc implementation shall provide the following phases in order:

1. **Lexical analysis** — produces a flat token stream; no whitespace tokens
2. **Parsing** — produces an AST; fails with structured E1xxx/E2xxx diagnostic on grammar violation
3. **Import resolution** — resolves all imports to interface files; fails with E2030 on missing module
4. **Name resolution** — resolves all identifier references to declarations; fails with E3011/E3012
5. **Type checking** — enforces all type rules from Section 12.7; fails with E4xxx diagnostics
6. **Arena validation** — verifies arena escape rules from Section 14.2; fails with E5001
7. **IR lowering** — produces toke IR in SSA form with explicit types
8. **LLVM IR emission** — produces LLVM IR from toke IR
9. **Native code generation** — invokes LLVM to produce native binary
10. **Interface emission** — produces `.tki` interface file for each compiled module
11. **Structured diagnostics emission** — all errors and warnings written to stderr in schema-conforming format

### 17.2 Compilation Targets

A conforming implementation shall support at minimum:

- x86-64 Linux (ELF)
- ARM64 Linux (ELF)
- ARM64 macOS (Mach-O)

Additional targets (WASM, Windows PE) may be supported as extensions.

### 17.3 Determinism

Given the same source files, compiler version, target architecture, and flags, a conforming compiler shall:
- produce identical diagnostics (same error codes, same locations)
- produce semantically equivalent output (same observable behaviour under the toke memory model)

Output binary byte-identity is not required (link addresses may vary); semantic equivalence is required.

### 17.4 Exit Codes

| Exit code | Meaning |
|-----------|---------|
| 0 | Compilation succeeded; all artefacts emitted |
| 1 | Compilation failed; diagnostics emitted to stderr |
| 2 | Internal compiler error; bug report should be filed |
| 3 | Usage error (invalid flags, missing input files) |

### 17.5 Performance Requirements [I]

The compiler should complete the following within the stated time on reference hardware (Apple M4, single core):

| Task | Target |
|------|--------|
| Lex + parse a 200-token .tk file | < 2ms |
| Full compilation of a 200-token .tk file to native binary | < 50ms |
| Incremental recompile (changed file only, interfaces cached) | < 5ms |
| Type-check a 50,000-file corpus | < 60 seconds |

These targets ensure the compiler runs synchronously within LLM API call timeouts.

### 17.6 Standard Command-Line Interface

```
tkc [flags] <source-files>

Flags:
  --target <arch-os>    compilation target (default: host)
  --out <path>          output binary path
  --emit-ir             emit LLVM IR alongside binary
  --emit-interface      emit .tki interface files
  --check               type-check only, no code generation
  --legacy              use legacy profile (80-char syntax, uppercase keywords)
  --diag-json           emit diagnostics as JSON (default: true)
  --diag-text           emit diagnostics as human-readable text
  --version             print compiler version and exit
```

---

## 18. Structured Diagnostics Specification [N]

Structured diagnostics are mandatory. A conforming compiler shall emit all errors, warnings, and informational messages in the schema defined below. Free-form text messages shall not be the primary diagnostic channel.

### 18.1 Diagnostic Schema

Every diagnostic record shall conform to the following schema:

```json
{
  "schema_version": "1.0",
  "diagnostic_id":  "<string: unique ID for this diagnostic instance>",
  "error_code":     "<string: stable code from Appendix A, e.g. E4020>",
  "severity":       "<error|warning|info>",
  "phase":          "<lex|parse|import_resolution|name_resolution|type_check|arena_check|ir_lower|codegen>",
  "message":        "<string: human-readable summary>",
  "file":           "<string: source file path>",
  "pos":            "<integer: byte offset from start of file>",
  "line":           "<integer: 1-based line number>",
  "column":         "<integer: 1-based column number>",
  "span_start":     "<integer: byte offset of error start>",
  "span_end":       "<integer: byte offset of error end>",
  "context":        ["<string: source lines surrounding the error>"],
  "expected":       "<string: what was expected>",
  "got":            "<string: what was found>",
  "fix":            "<string: suggested correction, if mechanically derivable>"
}
```

The `fix` field shall be populated whenever the correction is deterministic. Examples of mechanically derivable fixes:
- wrong type accessor: `r.str(id)` when field is `u64` -> `fix: r.u64(id)`
- missing error propagation: unchecked partial call -> `fix: add !$errorvariant`
- wrong variant name: misspelled variant -> `fix: $correct_variant_name`
- missing import: unresolved identifier that matches a stdlib module -> `fix: add i=alias:std.module`

### 18.2 Example Diagnostic

```json
{
  "schema_version": "1.0",
  "diagnostic_id": "diag-004819",
  "error_code": "E4020",
  "severity": "error",
  "phase": "type_check",
  "message": "argument type mismatch in struct field initialiser",
  "file": "api/user.tk",
  "pos": 247,
  "line": 12,
  "column": 8,
  "span_start": 242,
  "span_end": 258,
  "context": [
    "10: f=getuser(id:u64):$user!$usererr{",
    "11:   r=db.one(sql;@(id))!$usererr.$dberr;",
    "12:   <$user{id:r.str(id);name:r.str(name)}",
    "13: };"
  ],
  "expected": "u64",
  "got": "str",
  "fix": "r.u64(id)"
}
```

### 18.3 Diagnostic Stability Contract

- `error_code` values are stable across patch versions; minor versions may add new codes but shall not change existing meanings
- `fix` field values are stable within a compiler version
- The schema version is `1.0` for all version 0.1 diagnostics

Any automated repair system may depend on `error_code` and `fix` being stable between patch releases of the same minor version.

### 18.4 Runtime Trap Schema

Runtime traps emit the following record to stderr and exit with code 2:

```json
{
  "schema_version": "1.0",
  "trap_code": "<string: RT001-RT007>",
  "message": "<string>",
  "file": "<string: source file where trap originated>",
  "line": "<integer>",
  "function": "<string: function name>",
  "arena_depth": "<integer>",
  "context": ["<string: call stack frames>"]
}
```

---

## 19. Tooling Protocol [N]

The tooling protocol defines a machine-callable interface for driving the tkc compiler programmatically. It supports the generate-compile-inspect-repair loop without shell invocation.

### 19.1 Transport

The tooling protocol is transport-agnostic. Reference implementations shall support:
- JSON over stdin/stdout (primary)
- JSON-RPC 2.0 over TCP (optional)

### 19.2 Operations

| Operation | Description |
|-----------|-------------|
| `parse` | Lex and parse source, return AST or parse errors |
| `type_check` | Full type check through phase 6, return diagnostics |
| `compile` | Full compilation to native binary |
| `emit_interface` | Emit `.tki` interface file for a module |
| `run` | Compile and execute with captured stdout/stderr |
| `run_tests` | Compile and run test functions, return test results |
| `emit_diagnostics` | Return all diagnostics for a source file without compiling |

### 19.3 Request Schema

```json
{
  "operation":   "<string: operation name>",
  "request_id":  "<string: caller-assigned unique ID>",
  "source":      "<string: source code text>",
  "source_path": "<string: path for error reporting>",
  "target":      "<string: compilation target, optional>",
  "phase":       "<string: stop after this phase, optional>",
  "flags":       ["<string>"]
}
```

### 19.4 Response Schema

Every operation returns:

```json
{
  "request_id":   "<string: echoed from request>",
  "status":       "<ok|error>",
  "tool_version": "<string: tkc version>",
  "elapsed_ms":   "<integer>",
  "diagnostics":  ["<diagnostic record per Section 18.1>"],
  "artefacts":    {
    "binary_path":    "<string, if compiled>",
    "interface_path": "<string, if emitted>",
    "ir_path":        "<string, if emitted>"
  },
  "stdout":       "<string, if run>",
  "stderr":       "<string, if run>",
  "exit_code":    "<integer, if run>"
}
```

### 19.5 Repair Loop Integration [I]

A repair loop using the tooling protocol operates as follows:

```
1. POST {operation: "compile", source: generated_code}
2. IF status == "ok": accept program, add to corpus
3. IF status == "error":
   a. Extract first diagnostic with fix != ""
   b. Apply fix to source at (span_start, span_end)
   c. GO TO 1 (up to max_attempts)
4. IF max_attempts exceeded: escalate to LLM with full diagnostics array
```

The mechanical fix application in step 3b requires no LLM invocation and typically resolves type mismatches, missing error propagations, and wrong accessor calls within 1-2 rounds.

---

## 20. Conformance [N]

### 20.1 Conformance Levels

| Level | Requirement |
|-------|-------------|
| Level 1 — Lexical | Passes all lexical conformance tests |
| Level 2 — Parse | Passes all grammar conformance tests |
| Level 3 — Semantic | Passes all name resolution and type checking tests |
| Level 4 — Compile | Produces correct native binaries for all code generation tests |
| Level 5 — Diagnostic | Emits schema-conforming diagnostics for all error cases |
| Level 6 — Full | All of the above plus tooling protocol conformance |

A conforming implementation shall achieve Level 6.

### 20.2 Conformance Test Categories

The conformance suite shall include tests in the following categories:

**Lexical tests (L-series)**
- valid identifiers and keywords
- invalid characters in structural positions
- string literal escapes (valid and invalid)
- integer and float literal forms
- EOF handling

**Grammar tests (G-series)**
- all valid productions from Section 10
- all invalid constructs (parser rejects with correct error code)
- declaration ordering violations
- nested structure limits

**Name resolution tests (N-series)**
- unresolved imports
- unresolved identifiers
- duplicate declarations
- circular imports
- module path resolution

**Type checking tests (T-series)**
- all type rules from Section 12.7
- no-implicit-coercion cases
- exhaustive match enforcement
- error propagation type compatibility
- arena escape detection

**Code generation tests (C-series)**
- correct output for all constructs
- runtime trap conditions
- multi-file programs
- stdlib integration

**Diagnostic tests (D-series)**
- each error code in Appendix A has at least one test case
- diagnostic fields are populated correctly
- `fix` field accuracy for mechanically derivable cases

**Tooling protocol tests (P-series)**
- all operations in Section 19.2
- request/response schema conformance
- repair loop round-trip

### 20.3 Conformance Evaluation

Conformance is evaluated against normative expected outputs, not just successful compilation. Each test specifies:
- input source
- expected exit code
- expected diagnostic error codes (if any)
- expected `fix` field contents (for repair loop tests)
- expected programme output (for execution tests)

---

## 21. Benchmarking and Adoption Criteria [N]

This section defines the evidence required before toke is elevated to a broader industry standard. Adoption requires demonstrated, measurable performance against defined baselines and thresholds.

### 21.1 Evaluation Baselines

A candidate implementation shall be evaluated against the following baselines. These baselines define the evaluation context; they do not constitute claims about relative performance until benchmarks are run.

- Python 3.12 (idiomatic, type-annotated)
- TypeScript 5.x (strict mode)
- C (idiomatic, GCC -O2)
- Java 21 (idiomatic)
- A structured AST representation (JSON)
- One low-level IR (WASM textual format)

### 21.2 Benchmark Task Categories

**Category D2C — Algorithmic (single file)**
Standard computational problems requiring correct logic: sorting, graph traversal, string processing, arithmetic. Hidden tests include edge cases and performance constraints. These tasks validate single-function generation quality.

**Category C2C — System-level (multi-file)**
Multi-file projects: HTTP API with database backend, data pipeline with async I/O (async deferred but structure tested), CRUD service. Tests multi-module interface resolution and cross-file type consistency.

**Category M2C — Maintenance**
Bug-fix and refactoring tasks: present code with subtle bugs, measure whether the LLM can patch it via the repair loop. Tests diagnostic quality and repair-loop effectiveness.

**Category I2C — Interop**
Tasks requiring FFI or multi-language integration (deferred for full specification; test protocol integration in v0.1).

### 21.3 Required Benchmark Metrics

| Metric | Definition |
|--------|------------|
| Token count | Total LLM tokens consumed (input + output) per task |
| First-pass compile success | % of generations that compile without error on first attempt |
| Pass@1 | % of compilable first-attempt programs that pass all hidden tests |
| Pass@k | % of tasks solved within k generation attempts |
| Repair iterations | Mean number of compile-fix cycles before passing tests |
| Total cost | Token count x iterations, normalised |
| Multi-file success | % of Category C2C tasks completed correctly |
| Diagnostic usefulness | % of errors with a populated `fix` field that leads to a correct repair |
| Runtime performance | Geometric mean of execution time ratio vs native baseline |
| Binary size | Geometric mean of binary size ratio vs native baseline |

### 21.4 Minimum Evidence Threshold

A proposal for formal standardisation shall demonstrate:

- token reduction >= 15% on the D2C benchmark suite using the purpose-built tokenizer, measured against the evaluation baselines in Section 21.1
- Pass@1 >= 70% on D2C tasks
- Pass@3 >= 80% on D2C tasks
- stable compiler diagnostics (schema conformance 100%)
- reproducible results across at least two independent LLM families
- functioning multi-file ecosystem prototype (Category C2C pass rate >= 60%)
- no critical ambiguity in core semantics (zero grammar ambiguity, zero type rule contradictions)

### 21.5 Go/No-Go Gates

The project plan defines four gates. Each gate has a pass criterion; failure triggers a defined response.

**Gate 1 (Month 8):** Token reduction >= 10% on held-out D2C tasks; first-pass compile success >= 60%. Failure: halt language development and pivot to typed-IR approach only.

**Gate 2 (Month 14):** Extended language features maintain token efficiency; trained 7B model achieves >= 65% Pass@1 on D2C tasks. Failure: redesign extended feature set before proceeding.

**Gate 3 (Month 26):** Two or more independent LLM families achieve >= 70% Pass@1 on D2C tasks; self-improvement loop demonstrably improving corpus quality over successive iterations. Failure: re-evaluate standard prospects.

**Gate 4 (Month 32):** Benchmark evidence meets all thresholds in Section 21.4; formal specification complete; conformance suite published; consortium proposal ready. Failure: distill findings into a typed-IR standard instead.

---

## 22. Security and Safety Requirements [N]

### 22.1 Static Safety

The compiler shall reject the following statically where detectable:

- out-of-bounds constant-index violations (E4026)
- type-invalid operations that cannot produce well-typed results (E4020-E4029)
- unresolved imports (E2030)
- invalid error type compatibility in `!` propagation (E3020-E3022)
- arena escape violations (E5001)
- circular import dependencies (E2031)
- uninitialised binding use (E3015)

### 22.2 Runtime Safety

Where static rejection is not possible, the runtime shall:

- check array bounds on every access and trap with RT001 on violation
- trap on integer division by zero (RT002)
- trap on integer overflow when using checked arithmetic operations (RT003)
- trap on arena boundary violations (RT005)

All traps produce a structured trap record (Section 18.4) and exit with code 2. Traps shall not produce undefined behaviour; the programme terminates cleanly.

### 22.3 Repair-Loop Safety

Structured diagnostics shall not rely on unstable free-form prose. The `error_code` field is the primary machine-readable signal. The `fix` field shall be populated only when the suggested fix is deterministically correct; an incorrect suggested fix is a conformance defect.

### 22.4 Sandboxing Properties [I]

Because toke's runtime calls are limited to explicit stdlib operations, a toke programme without FFI:

- has no direct system call access outside stdlib-mediated I/O
- has no access to arbitrary memory addresses
- has no ability to load dynamic libraries at runtime
- has no metaprogramming or code execution facilities

These properties make toke programmes safer to execute in LLM-driven generation loops without human review of each generated programme.

---

## 23. Reference Implementation Requirements [N]

The reference implementation (`tkc`) shall include:

- compiler frontend (lexer, parser, name resolver, type checker, arena validator)
- LLVM IR backend
- structured diagnostic emitter (Section 18 schema)
- tooling protocol server (Section 19)
- canonical interface file emitter (`.tki` format)
- conformance suite runner
- benchmark harness integrating with the parallel differential testing pipeline
- example module library (stdlib reference implementations)

The reference implementation is not the standard. Where the specification and the implementation conflict, the specification governs. The reference implementation serves as the principal executable interpretation of ambiguous cases until the formal semantics are complete.

The reference implementation shall be:
- written in C with no external runtime dependencies (excluding LLVM as a build dependency)
- compilable on macOS ARM64, Linux x86-64, and Linux ARM64 with no modification
- under 10,000 lines of C for the frontend (excluding LLVM backend glue)
- distributed under an open-source licence permitting commercial use and modification

---

## 24. Deferred and Promoted Items [N]

The following items were originally deferred from version 0.1. This section tracks their current status as of v0.3 and assigns target milestones where applicable. Items marked **promoted** have moved into the normative specification.

### 24.1 Concurrency and Async Semantics

**Status: Deferred — Target: v0.5+**

Async/await, goroutine-style lightweight threads, message passing, channels, and structured concurrency are all deferred. Version 0.1 is single-threaded. The concurrency model must be chosen consistently with the arena memory model and specified in a future version.

### 24.2 Foreign Function Interface [N]

**Status: Experimental — documents current implementation. Subject to change in v0.4.**

This section specifies the foreign function interface (FFI) that allows toke programmes to call functions defined in C or other languages exposing a C-compatible ABI.

#### 24.2.1 Extern Function Declarations

An extern function is declared using the standard `f=` syntax with parameter types and a return type, but **without a body block**. The absence of `{` after the return type signals to the parser that the declaration is extern.

Grammar:

```
f=name(param1:type1; param2:type2):returnType;
```

Examples:

```
f=puts(s:$str):i64;
f=clock():i64;
f=write(fd:i64; buf:$str; count:i64):i64;
```

The compiler emits an LLVM `declare` for each extern declaration. The linker resolves the symbol at link time against the C library or any object file provided on the link command line.

An extern declaration SHALL NOT contain a body. A body-bearing `f=` declaration is always a toke-internal function.

#### 24.2.2 The i64 ABI

All values crossing the C boundary are represented as `i64` (64-bit signed integer) unless one of the following exceptions applies:

| toke type | C boundary type | Notes |
|-----------|----------------|-------|
| `i64`     | `int64_t`      | Direct; no conversion. |
| `f64`     | `double`       | Passed via `memcpy`-style bitcast (see 24.2.4). |
| `$str`    | `const char*`  | Pointer encoded as `i64` via `intptr_t` cast. |
| `bool`    | `int`          | `true` = 1, `false` = 0; zero-extended to `i64`. |
| `void`    | `void`         | No return value. |

This "everything is i64" convention simplifies code generation and keeps the internal IR uniform. The compiler inserts the necessary `inttoptr`, `ptrtoint`, or bitcast instructions at FFI call boundaries.

#### 24.2.3 Calling Conventions

Extern functions use the **platform-default C calling convention**:

- **x86-64 Linux/macOS:** System V AMD64 ABI.
- **ARM64 (AArch64):** AAPCS64.

Internal toke functions use LLVM `fastcc` (fast calling convention), which permits tail-call optimisation and register-based parameter passing beyond what the C ABI guarantees.

LLVM handles all register assignment, stack spilling, and red-zone management for both conventions. The toke compiler does not perform manual register allocation.

At an FFI call site the compiler emits a `call` instruction with the default (C) calling convention regardless of the surrounding function's convention. No thunk or trampoline is required; LLVM lowers the convention mismatch automatically.

#### 24.2.4 Type Marshalling

The following table specifies the exact mapping between toke types and their C representations at the FFI boundary.

| toke type | C type | Marshalling rule |
|-----------|--------|-----------------|
| `i64` | `int64_t` | Identity. No conversion. |
| `$str` | `const char*` | The string's data pointer is cast to `i64` via `ptrtoint`. On the C side it is a null-terminated `const char*`. Incoming `const char*` values are cast back via `inttoptr`. |
| `bool` | `int` | `true` maps to integer `1`; `false` maps to integer `0`. C return values are compared `!= 0` to produce a toke `bool`. |
| `f64` | `double` | Bitcast via `memcpy` semantics (LLVM `bitcast` between `i64` and `double`). This preserves IEEE 754 bit patterns without floating-point conversion. |
| struct | pointer | Structs are passed by pointer. The pointer is cast to `i64` on the toke side. The C side receives a pointer to the struct layout. |
| array | length-prefixed layout | Arrays are represented as a length-prefixed block: an `i64` length followed by contiguous elements. The pointer to the block is cast to `i64`. |

#### 24.2.5 Ownership Rules

Memory ownership at FFI boundaries follows these rules:

1. **C-allocated memory is not managed by the toke arena.** The toke runtime will not free, relocate, or track memory obtained from `malloc`, `mmap`, or any other C allocator.

2. **Arena-allocated pointers must not escape arena scope.** Passing an arena-allocated pointer to a C function that stores it beyond the current arena's lifetime is undefined behaviour.

3. **Strings returned from C are used directly.** When a C function returns a `const char*`, toke uses the pointer as-is without copying the data into the arena. The caller is responsible for ensuring the pointer remains valid for the duration of its use.

#### 24.2.6 Safety

FFI calls are **inherently unsafe**. The toke compiler performs no bounds checking, null-pointer checking, or integer-overflow checking on values crossing the FFI boundary. Specifically:

- A null pointer returned from C and dereferenced in toke causes undefined behaviour.
- Buffer overruns in C-allocated memory are not detected.
- Type mismatches between the toke extern declaration and the actual C function signature produce undefined behaviour at the ABI level.

A future version of the specification may require an `unsafe` annotation on extern declarations or on call sites to make the hazard explicit in source code.

### 24.3 Package System [N]

**Status: Design complete — ADR-0004 — Target: v0.6+**

This section specifies the toke package system: manifest format, namespace conventions, version resolution algorithm, distribution mechanism, import integration, and lock file format. Implementation is scheduled for v0.6 or later; this section is normative so that early adopters and tooling authors can build against a stable contract.

#### 24.3.1 Package Manifest (`pkg.toml`)

Every toke project that declares external dependencies SHALL contain a `pkg.toml` file at the project root. The manifest uses TOML format and contains two required tables:

```toml
[package]
name = "myapp"
version = "1.0.0"

[dependencies]
webframework = ">=0.3.0"
jsonparser = "1.2.0"
```

**`[package]` table — required fields:**

| Field     | Type   | Description |
|-----------|--------|-------------|
| `name`    | string | Package name. Lowercase ASCII letters and digits only. No underscores, hyphens, or dots. Must match `[a-z][a-z0-9]{0,63}`. |
| `version` | string | SemVer 2.0 version string (`MAJOR.MINOR.PATCH`). Pre-release and build metadata suffixes are permitted per SemVer but are ignored during resolution. |

**`[dependencies]` table — optional:**

Each key is a package name; each value is a version constraint string. Supported constraint forms:

| Form          | Meaning |
|---------------|---------|
| `"1.2.0"`     | Exactly version 1.2.0 (equivalent to `"=1.2.0"`). |
| `">=1.2.0"`   | Version 1.2.0 or any later version. |
| `">=1.2.0 <2.0.0"` | At least 1.2.0 and strictly less than 2.0.0. |

The constraint syntax is intentionally minimal. Tilde ranges (`~`), caret ranges (`^`), and wildcard ranges (`*`) are not supported in v0.6. A future version may add them.

A project with no external dependencies MAY omit `pkg.toml` entirely. The compiler SHALL treat the absence of `pkg.toml` as an empty dependency set.

#### 24.3.2 Namespace Conventions

Packages occupy two disjoint namespaces:

| Prefix   | Scope | Examples |
|----------|-------|----------|
| `std.*`  | Standard library modules shipped with the compiler. | `std.http`, `std.json`, `std.str` |
| `pkg.*`  | Third-party packages resolved from `pkg.toml`. | `pkg.webframework`, `pkg.jsonparser` |

A source file imports a third-party package using the `pkg.` prefix:

```toke
m=myapp;
i=wf:pkg.webframework;
i=jp:pkg.jsonparser;
i=http:std.http;
```

The `pkg.` prefix is syntactically identical to the `std.` prefix used for stdlib imports. The compiler distinguishes them by prefix and resolves `pkg.*` imports against the dependency cache rather than the stdlib directory.

Package names within `pkg.*` SHALL be globally unique. The central index (when established) is the authority for name reservation. Until a central index exists, uniqueness is enforced by convention.

#### 24.3.3 Version Resolution — Minimum Version Selection (MVS)

toke uses Minimum Version Selection (MVS) as defined by Russ Cox (2018). MVS guarantees:

1. **Determinism.** Given identical `pkg.toml` files across the dependency graph, the resolved versions are identical on every machine and every run. No SAT solver or heuristic is involved.

2. **Minimum versions.** For each dependency, MVS selects the minimum version that satisfies all constraints in the transitive dependency graph. It never selects a newer version than required.

3. **No lock file required for correctness.** Because MVS is deterministic, a lock file is technically redundant. However, `pkg.lock` is still produced for auditability and to record the exact commit hashes used (see 24.3.5).

**Algorithm sketch:**

1. Parse `pkg.toml` from the project root. Collect the set of direct dependencies with their version constraints.
2. For each dependency, fetch its own `pkg.toml` (from the tagged version in its git repository). Collect transitive dependencies recursively.
3. Build a requirement list: for each package name, the set of all version constraints from all dependents.
4. For each package, select the minimum version that satisfies every constraint in the requirement list.
5. If no version satisfies all constraints for a package, emit diagnostic E9010 (dependency conflict) and halt.

#### 24.3.4 Distribution — Git-Based

Each package is a git repository. Tagged versions correspond to SemVer tags:

```
v1.0.0
v1.2.3
v2.0.0-rc.1
```

Tags MUST use the `v` prefix followed by the SemVer version. The compiler strips the `v` prefix when matching against version constraints.

**Repository layout requirements:**

A valid toke package repository SHALL contain at its root:

- `pkg.toml` with a `[package]` table declaring `name` and `version`.
- One or more `.tk` source files comprising the package modules.
- A top-level module file whose name matches the package name (e.g., `webframework.tk` for package `webframework`), serving as the package entry point.

**Optional central index:**

A future central index at `pkg.toke.dev` (or similar) MAY map package names to git repository URLs. The index is a convenience layer; it is not required for resolution. Users MAY specify repository URLs directly in an extended `[dependencies]` table:

```toml
[dependencies.webframework]
version = ">=0.3.0"
git = "https://github.com/example/toke-webframework.git"
```

When no `git` field is present, the compiler SHALL consult the central index to resolve the package name to a repository URL.

#### 24.3.5 Lock File (`pkg.lock`)

After successful resolution, the compiler writes `pkg.lock` to the project root. The lock file is TOML and records the exact resolved version and git commit hash for every dependency (direct and transitive):

```toml
[package.webframework]
version = "0.3.0"
commit = "a1b2c3d4e5f6a1b2c3d4e5f6a1b2c3d4e5f6a1b2"

[package.jsonparser]
version = "1.2.0"
commit = "f6e5d4c3b2a1f6e5d4c3b2a1f6e5d4c3b2a1f6e5"
```

The lock file SHALL be committed to version control. When `pkg.lock` is present, `tkc pkg fetch` SHALL use the recorded commit hashes rather than re-resolving, ensuring bit-for-bit reproducible builds.

If `pkg.toml` is modified (dependency added, removed, or constraint changed), the lock file is invalidated and must be regenerated via `tkc pkg resolve`.

#### 24.3.6 Import Syntax Integration

The import declaration for packages follows the existing `i=alias:path` syntax (Section 15.2):

```toke
i=wf:pkg.webframework;
```

This imports the entry-point module of package `webframework` under the local alias `wf`. Sub-modules within a package are accessed via dotted paths:

```toke
i=router:pkg.webframework.router;
i=mw:pkg.webframework.middleware;
```

The compiler resolves `pkg.webframework.router` to the file `router.tk` within the `webframework` package directory in the local cache.

#### 24.3.7 Local Cache

Fetched packages are stored in a local cache at `~/.toke/pkg/`. The cache layout is:

```
~/.toke/pkg/
  webframework/
    0.3.0/
      pkg.toml
      webframework.tk
      router.tk
      middleware.tk
      ...
  jsonparser/
    1.2.0/
      pkg.toml
      jsonparser.tk
      ...
```

Each version occupies its own directory. Multiple versions of the same package MAY coexist in the cache, but MVS guarantees that only one version of each package is selected per build.

#### 24.3.8 Diagnostics

| Code  | Severity | Meaning |
|-------|----------|---------|
| E9010 | error    | Dependency version conflict: no version of package `X` satisfies all constraints. |
| E9011 | error    | Package not found: `X` is not in the central index and no `git` URL is specified. |
| E9012 | error    | Malformed `pkg.toml`: missing required field or invalid TOML syntax. |
| E9013 | error    | Git fetch failed: could not clone or fetch repository for package `X`. |
| E9014 | error    | Version tag not found: package `X` has no tag matching the resolved version. |
| W9010 | warning  | Lock file out of date: `pkg.toml` has changed since `pkg.lock` was generated. |

### 24.4 Formal Memory Model with Ownership Annotations

**Status: Deferred (downstream of concurrency) — Target: After v0.5**

The arena model is informally specified in Section 14. A formal operational semantics with proof obligations for memory safety is deferred. This item is downstream of the concurrency model (24.1) and will be addressed after that design is settled.

### 24.5 Debugger Metadata

**Status: Deferred — LLVM/DWARF defaults apply**

DWARF or equivalent metadata for interactive debugging, source maps for IDE integration, and variable inspection are deferred. In the interim, standard LLVM-generated DWARF metadata applies to compiled binaries.

### 24.6 Canonical Binary IR

**Status: Deferred — LLVM bitcode is interim format**

A typed binary intermediate representation for toke programmes suitable for distribution, static analysis, and machine-to-machine exchange without recompilation is deferred. LLVM bitcode serves as the interim binary format.

A design document for the `.tkir` binary IR format — covering file layout, type encoding, register-based SSA instruction set (~35 opcodes), export/import tables, and a four-phase migration path — is maintained at `docs/architecture/binary-ir-design.md`.

### 24.7 Tokenizer Vocabulary

**Status: PROMOTED TO NORMATIVE — Target: v0.3**

The toke BPE tokenizer vocabulary is a first-class language artifact. The canonical vocabulary lists the keywords, common stdlib identifiers, and common operator sequences that any toke-aware tokenizer should treat as atomic tokens.

The 8K BPE tokenizer has been trained on the toke corpus. Phase 2 retraining is pending completion of expanded corpus coverage. The full vocabulary specification will be maintained as a companion document referenced from this specification.

### 24.8 Closures and Higher-Order Functions

**Status: IMPLEMENTED (function references) — Full closures deferred to v0.4**

Function references are supported via `&name` syntax. The `&` prefix applied to a function name produces a first-class function value that can be passed as an argument, stored in a binding, or returned from a function.

**Syntax:** `&functionname` — produces a value of function type matching the referenced function's signature.

**Example:**
```
f=apply(fn:&(i64):i64;x:i64):i64{
  <fn(x)
};

f=double(n:i64):i64{
  <n*2
};

f=main():i64{
  let r = apply(&double;5);
  <r
};
```

The `&` symbol in toke is used exclusively as the function reference prefix. It is not a bitwise operator (bitwise operations are deferred to v0.5+).

Full closures with environment capture remain deferred.

Full closures capturing mutable bindings from enclosing scopes are deferred due to interactions with the arena model (captured mutable bindings and arena lifetime). These are targeted for v0.4.

### 24.9 Generics and Parametric Types

**Status: Deferred**

Generic type parameters beyond built-in collection types are deferred to a future version.

### 24.10 Option Type

**Status: Deferred — Target: v0.4**

A built-in `$option` type is deferred. In version 0.1, optionality is expressed through sum types: `t=$maybeuser{$some:$user;$none:bool}`. The `$option` type is a natural fit with `$some`/`$none` tags and is targeted for v0.4.

### 24.11 Companion Files (.tkc) [N]

**Status:** First-class language mechanism. Format reserved for v0.4.

Every `.tk` source file may have an associated **companion file** with the `.tkc` extension. Companion files live alongside their source file and share the same base name (e.g., `main.tk` + `main.tkc`).

Companion files are the designated mechanism for:
- Human-readable documentation of toke programs
- API documentation and metadata
- Any non-code annotations associated with a source file

The internal format of `.tkc` files is reserved for specification in v0.4. Until the format is specified, implementations shall not assign semantics to `.tkc` file contents, but shall recognise `.tkc` files as belonging to the toke toolchain and shall not treat them as source input.

Companion files are part of the toke language design. Source code is kept comment-free (see Section 8.10); all documentation lives in companion files.

### 24.13 Multimodal LLM Code Generation [I]

**Status:** Minimal contract. Formal protocol reserved for a future version.

Toke programs may be authored by a single LLM or by a code-and-content division of labour:

- The **code channel** produces structural toke source (`.tk` files). It is responsible for syntax, types, control flow, and module structure.
- The **content channel** produces string literals, i18n keys, template content, and companion file content (`.tkc` files — see Section 24.11).

The two channels must agree on identifiers and tag names. A disagreement between code-channel identifiers and content-channel string references is an error caught at compile time (when the identifier is used) or at runtime (when the string is resolved).

No formal protocol for the division is specified in v0.3. Implementations may use any mechanism — separate prompts to the same model, separate models, or a single model with structured output — provided the contract above is maintained.

### 24.14 1B Purpose-Built Model

**Status: Planned — Target: After Gate 2**

The long-term target for toke AI code generation is a 1B parameter model trained from scratch on the toke corpus — not fine-tuned from an existing LLM. The phased approach is:

- **Phase 1 (complete):** Fine-tune existing LLMs (7B Qwen) on toke corpus. Gate 1 PASS: 63.7% Pass@1.
- **Phase 2 (in progress):** Retrain tokenizer on default syntax, regenerate corpus, curriculum learning. Gate 2: fine-tuned 7B outperforms baseline on default-syntax generation.
- **Phase 3 (planned):** Train a 1B parameter model from scratch on comment-free, 55-character default-syntax toke programs using the purpose-built tokenizer.

The hypothesis is that a smaller model purpose-built for toke will outperform larger fine-tuned models on toke-specific tasks, because it learns toke as a native language rather than adapting weights pre-trained on Python, C, and natural language. The 1B model's evaluation target is to exceed the fine-tuned 7B on Pass@1, compilation rate, and token efficiency on toke generation benchmarks.

Architecture design, corpus requirements, training infrastructure, and evaluation criteria are tracked in the project backlog (Epic 81).

---

## 25. Governance and Versioning [N]

### 25.1 Version Scheme

The toke specification is versioned semantically as `MAJOR.MINOR.PATCH`:

- **Major** — incompatible language or tooling changes; existing source may require migration
- **Minor** — backward-compatible additions; existing source remains valid
- **Patch** — wording clarifications and defect corrections with no normative behaviour change

Version 0.1 is the initial draft specification. Version 1.0 will be designated when the ecosystem proof is complete and the conformance suite passes for at least two independent compiler implementations.

### 25.2 Stability Guarantees by Version

- `0.x` — specification is unstable; any aspect may change between minor versions
- `1.x` — source language (Layer A) is stable; breaking changes require a major version bump
- `1.x` — diagnostic error codes are stable; new codes may be added in minor versions
- `1.x` — tooling protocol schema is stable for existing fields; new fields may be added

### 25.3 Change Process

Changes to the normative specification shall be:

1. Proposed as an issue in the public specification repository
2. Discussed with a minimum 30-day public comment period for minor/major changes
3. Validated against the conformance suite before acceptance
4. Documented in the change log with rationale

Patch changes may be made with a 7-day review period.

### 25.4 Open Governance

The specification repository shall be public. All normative and informative sections shall be clearly marked. Reference tests shall be openly licensed. No single organisation shall have unilateral authority over the specification after version 1.0.

---

## Appendix A — Error Code Registry [N]

Error codes are stable across patch versions. All codes beginning with `E` are errors; `W` are warnings; `L` are linter notices.

### Lexical Errors (E10xx)

| Code | Meaning |
|------|---------|
| E1001 | Unrecognised escape sequence in string literal |
| E1002 | Unterminated string literal at end of file |
| E1003 | Character outside toke character set in structural position |
| E1004 | Identifier begins with digit |
| E1005 | Invalid UTF-8 byte sequence in source file |

### Syntax/Grammar Errors (E20xx)

| Code | Meaning |
|------|---------|
| E2001 | Declaration out of order (imports after types, etc.) |
| E2002 | Missing module declaration |
| E2003 | Multiple module declarations in one file |
| E2004 | Missing semicolon terminator |
| E2005 | Duplicate exported name across files in same module |
| E2006 | Wildcard import prohibited |
| E2007 | Unexpected token — expected production does not match |
| E2008 | Missing closing delimiter |
| E2009 | Empty function body |
| E2010 | Pointer type `*$t` used outside extern function |
| E2011 | Mixed struct and sum fields in type declaration |
| E2015 | Recursive struct without indirection |
| E2020 | Circular static initialisation |
| E2030 | Unresolved import — module not found |
| E2031 | Circular import dependency |

### Name Resolution Errors (E30xx)

| Code | Meaning |
|------|---------|
| E3001 | Error propagation `!` in total function |
| E3005 | Missing return in non-void function |
| E3010 | Assignment to immutable binding |
| E3011 | Unresolved identifier |
| E3012 | Ambiguous identifier (multiple candidates) |
| E3015 | Potentially uninitialised binding used |
| E3020 | Error propagation `!` variant not in function error type |
| E3021 | Error payload type incompatible with variant payload |
| E3022 | Error propagation in function without declared error type |

### Type Errors (E40xx)

| Code | Meaning |
|------|---------|
| E4001 | Non-bool condition in `if` |
| E4010 | Non-exhaustive match |
| E4011 | Match arms return different types |
| E4020 | Argument type mismatch in function call |
| E4021 | Return expression type mismatch |
| E4025 | Field not found in struct |
| E4026 | Array index not of integer type |
| E4030 | Cast to incompatible type |
| E4031 | Implicit coercion prohibited |

### Arena Errors (E50xx)

| Code | Meaning |
|------|---------|
| E5001 | Arena-allocated value escapes arena boundary |
| E5002 | Returned pointer to arena-local allocation |

### Warnings (W10xx)

| Code | Meaning |
|------|---------|
| W1001 | Potentially truncating cast |
| W1002 | Unused binding |
| W1003 | Unused import |
| W1004 | Unreachable code after unconditional return |

### Linter Notices (L00xx)

| Code | Meaning |
|------|---------|
| L0001 | File exports more than one primary construct |
| L0002 | Function longer than 50 statements |
| L0003 | Identifier exceeds 40 characters |

---

## Appendix B — Diagnostic Schema (Normative) [N]

The following JSON Schema defines the normative diagnostic record format for version 0.1:

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://tokelang.dev/spec/v0.1/diagnostic",
  "title": "toke Diagnostic Record",
  "type": "object",
  "required": [
    "schema_version", "diagnostic_id", "error_code",
    "severity", "phase", "message",
    "file", "pos", "line", "column",
    "span_start", "span_end",
    "context", "expected", "got"
  ],
  "properties": {
    "schema_version": { "type": "string", "const": "1.0" },
    "diagnostic_id":  { "type": "string" },
    "error_code":     { "type": "string", "pattern": "^[EWL][0-9]{4}$" },
    "severity":       { "type": "string", "enum": ["error", "warning", "info"] },
    "phase":          { "type": "string", "enum": [
                        "lex", "parse", "import_resolution",
                        "name_resolution", "type_check",
                        "arena_check", "ir_lower", "codegen"
                       ]},
    "message":        { "type": "string" },
    "file":           { "type": "string" },
    "pos":            { "type": "integer", "minimum": 0 },
    "line":           { "type": "integer", "minimum": 1 },
    "column":         { "type": "integer", "minimum": 1 },
    "span_start":     { "type": "integer", "minimum": 0 },
    "span_end":       { "type": "integer", "minimum": 0 },
    "context":        { "type": "array", "items": { "type": "string" } },
    "expected":       { "type": "string" },
    "got":            { "type": "string" },
    "fix":            { "type": "string" }
  }
}
```

---

## Appendix C — Tooling Protocol Schema (Normative) [N]

### Request Schema

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://tokelang.dev/spec/v0.1/tooling-request",
  "type": "object",
  "required": ["operation", "request_id", "source"],
  "properties": {
    "operation":   { "type": "string",
                     "enum": ["parse","type_check","compile",
                              "emit_interface","run","run_tests",
                              "emit_diagnostics"] },
    "request_id":  { "type": "string" },
    "source":      { "type": "string" },
    "source_path": { "type": "string" },
    "target":      { "type": "string" },
    "phase":       { "type": "string" },
    "flags":       { "type": "array", "items": { "type": "string" } }
  }
}
```

### Response Schema

```json
{
  "$schema": "https://json-schema.org/draft/2020-12/schema",
  "$id": "https://tokelang.dev/spec/v0.1/tooling-response",
  "type": "object",
  "required": ["request_id", "status", "tool_version", "elapsed_ms", "diagnostics"],
  "properties": {
    "request_id":   { "type": "string" },
    "status":       { "type": "string", "enum": ["ok", "error"] },
    "tool_version": { "type": "string" },
    "elapsed_ms":   { "type": "integer", "minimum": 0 },
    "diagnostics":  { "type": "array" },
    "artefacts":    {
      "type": "object",
      "properties": {
        "binary_path":    { "type": "string" },
        "interface_path": { "type": "string" },
        "ir_path":        { "type": "string" }
      }
    },
    "stdout":     { "type": "string" },
    "stderr":     { "type": "string" },
    "exit_code":  { "type": "integer" }
  }
}
```

---

## Appendix D — Keyword and Symbol Reference [I]

### Declaration Keywords (Context-Sensitive)

| Keyword | Role | Notes |
|---------|------|-------|
| `m=` | Module declaration | `m` is a valid variable name inside functions |
| `f=` | Function definition | `f` is a valid variable name inside functions |
| `t=` | Type definition | `t` is a valid variable name inside functions |
| `i=` | Import declaration | `i` is a valid variable name inside functions |

### Reserved Keywords (9 keywords + 4 context keywords = 13 total)

| Keyword | Role | Notes |
|---------|------|-------|
| `if` | Conditional | Lowercase |
| `el` | Else branch | Follows `}` of `if` only |
| `lp` | Loop | The single loop construct |
| `br` | Break | Exits innermost `lp` |
| `let` | Binding | `let x=v` immutable; `let x=mut.v` mutable |
| `mut` | Mutable qualifier | Used as `mut.value` in binding only |
| `as` | Type cast | `expr as $targettype` |
| `rt` | Return (long form) | Equivalent to `<` |
| `mt` | Match expression | `mt expr { arms }` |

### Operators (19 symbols)

| Symbol | As operator | As delimiter |
|--------|-------------|--------------|
| `=` | equality in match | bind in declaration |
| `:` | type annotation | field separator in struct |
| `.` | member access | module path separator |
| `;` | statement terminator | argument/field separator |
| `(` | call open | group open |
| `)` | call close | group close |
| `{` | block open | struct literal open |
| `}` | block close | struct literal close |
| `+` | add | string concat |
| `-` | subtract | unary negate |
| `*` | multiply | pointer deref (FFI only) |
| `/` | divide | -- |
| `<` | return (short form) | less-than in comparison |
| `>` | greater-than | -- |
| `!` | error propagation | logical not |
| `\|` | -- | union type separator |
| `&&` | logical and | -- |
| `\|\|` | logical or | -- |
| `$` | type name sigil prefix | -- |
| `@` | array/map literal and type sigil | -- |
| `&` | function reference prefix | -- |
| `%` | modulo | -- |

> **Note:** The double-quote `"` appears in source as the string delimiter but is not a structural symbol — it is consumed by the lexer during string literal scanning and never produces a token. See Section 7 note.

---

## Appendix E — Reserved Identifiers [N]

The following identifiers are reserved and may not be used as user-defined names:

**Reserved keywords (Section 11.2):** `if` `el` `lp` `br` `let` `mut` `as` `rt` `mt`

**Predefined values:** `true` `false`

**Built-in types (scalar):** `u8` `u16` `u32` `u64` `i8` `i16` `i32` `i64` `f32` `f64` `bool` `str` `byte`

**Built-in type constructors:** `$ok` `$err` (variants of the built-in `$result` type)

**Special identifiers:** `arena` (used as block introducer), `len` (array length member — not reusable as a field name in user types)

**Standard library module aliases:** `std` (reserved as a top-level module path prefix; user modules may not use `std.x` paths)

---

## Appendix F — Legacy Profile [I]

The legacy profile is an 80-character variant of the toke language available via the `--legacy` compiler flag. It was the original development syntax and remains supported for backward compatibility.

### F.1 Character Set (80 Characters)

```
CLASS        CHARACTERS                                                 COUNT
──────────────────────────────────────────────────────────────────────────────
Lowercase    a b c d e f g h i j k l m n o p q r s t u v w x y z       26
Uppercase    A B C D E F G H I J K L M N O P Q R S T U V W X Y Z       26
Digits       0 1 2 3 4 5 6 7 8 9                                       10
Symbols      ( ) { } [ ] = : . ; + - * / < > ! |                       18
──────────────────────────────────────────────────────────────────────────────
TOTAL                                                                   80
```

### F.2 Differences from Default Syntax

| Feature | Default syntax | Legacy profile |
|---------|---------------|----------------|
| Declaration keywords | `m=` `f=` `t=` `i=` (lowercase) | `M=` `F=` `T=` `I=` (uppercase, reserved) |
| Type names | `$user`, `$str` | `User`, `Str` (uppercase-initial) |
| Array literal | `@(1; 2; 3)` | `[1; 2; 3]` |
| Array type | `@i64` | `[i64]` |
| Map literal | `@(1:10; 2:20)` | `[1:10; 2:20]` |
| Map type | `@(i64:str)` | `[i64:str]` |
| Array indexing | `arr.get(0)` | `arr[0]` |
| Struct literal | `$point{x: 1; y: 2}` | `Point{x: 1; y: 2}` |
| `$` and `@` | structural symbols | not available (E1003) |
| `[` and `]` | not available | array delimiters |

### F.3 Legacy Profile Keywords

In legacy mode, the four declaration keywords are uppercase reserved words:

| Keyword | Role |
|---------|------|
| `F` | function definition (reserved) |
| `T` | type definition (reserved) |
| `I` | import declaration (reserved) |
| `M` | module declaration (reserved) |

All other keywords (`if`, `el`, `lp`, `br`, `let`, `mut`, `as`, `rt`, `mt`) are identical in both modes.

### F.4 Scalar Type Names in Legacy Mode

| Default | Legacy |
|---------|--------|
| `str` | `Str` |
| `byte` | `Byte` |

All numeric types (`i64`, `u64`, `f64`, `bool`, etc.) are identical in both modes.

### F.5 Example — Legacy vs Default

**Legacy profile (`--legacy`):**
```
M=api.user;
T=User{id:u64;name:Str;email:Str};
F=getuser(id:u64):User!UserErr{
  r=db.one("SELECT id,name FROM users WHERE id=?";[id])!UserErr.DbErr;
  <User{id:r.u64(id);name:r.str(name)}
};
```

**Default syntax (no flag):**
```
m=api.user;
t=$user{id:u64;name:str;email:str};
f=getuser(id:u64):$user!$usererr{
  r=db.one("SELECT id,name FROM users WHERE id=?";@(id))!$usererr.$dberr;
  <$user{id:r.u64(id);name:r.str(name)}
};
```

### F.6 Compiler Flag

```
tkc --legacy source.tk        # compile in legacy mode
tkc source.tk                  # compile in default mode (no flag needed)
```

The deprecated flags `--phase1` and `--profile1` are aliases for `--legacy` and may be removed in a future version.

---

*toke specification version 0.1 — draft. All normative sections are marked [N]. All informative sections are marked [I]. Sections without a marker are informative by default.*

*Repository: https://github.com/tokelang/spec*
*Issues: https://github.com/tokelang/spec/issues*
*Change log: https://github.com/tokelang/spec/CHANGELOG.md*
