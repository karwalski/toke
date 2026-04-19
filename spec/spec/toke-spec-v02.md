# toke Language Specification
## Version 0.1 — Draft

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
7. Character Set (56 Characters)
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
- the complete character set (56 chars)
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
- full foreign function interface rules (Section 24.2)
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

## 7. Character Set (56 Characters) [N]

toke source uses exactly 56 characters. No character outside this set shall appear in toke source except within string literal content, where arbitrary UTF-8 is permitted.

### 7.1 Complete Character Table

```
CLASS        CHARACTERS                                                 COUNT
──────────────────────────────────────────────────────────────────────────────
Lowercase    a b c d e f g h i j k l m n o p q r s t u v w x y z       26
Digits       0 1 2 3 4 5 6 7 8 9                                       10
Symbols      ( ) { } = : . ; + - * / < > ! | $ @                       18
Reserved     ^ ~                                                         2
──────────────────────────────────────────────────────────────────────────────
TOTAL                                                                   56
```

The reserved characters `^` and `~` are not assigned in version 0.1 but are excluded from use to preserve their availability for version 0.2 extensions.

Note: The double-quote `"` appears in source as the string delimiter for string literals but is not a structural symbol — it is consumed by the lexer during string literal scanning and never produces a token. It is analogous to whitespace in this regard.

### 7.2 Excluded Characters

The following character classes are explicitly excluded from structural toke source:

- Uppercase letters (A-Z) — not used in structural positions; type references use the `$` sigil instead
- Square brackets `[` `]` — not used; array literals use `@(...)`, array types use `@$type`
- Whitespace (space U+0020, tab U+0009, carriage return U+000D, line feed U+000A) — structurally meaningless; permitted only within string literals
- Comments — no comment syntax is defined; comments are metadata stored outside source files
- `#`, `%`, `&`, `` ` ``, `\`, `'`, `,`, `?` — not assigned

The absence of whitespace as a structural element means the lexer produces a flat token stream with no position-dependent meaning. Two programs that differ only in whitespace between tokens are lexically identical.

Note: The `&` character does not appear alone, but the two-character sequence `&&` is a logical-and operator (see Section 7.4). Similarly `||` is logical-or, composed of two `|` characters.

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

With the purpose-built toke tokenizer:

- `$user`, `$str`, `$err` each tokenize as a single vocabulary token due to 100% co-occurrence of `$` with a type name
- `@(` tokenizes as a single token (always together)
- Common patterns such as `f=`, `!$err`, `<$res.ok` become single tokens
- Expected token density improvement: 2.5-4x fewer LLM tokens per program vs cl100k_base

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

An identifier:
- begins with a letter (a-z)
- continues with letters or digits (0-9)
- is case-sensitive
- must not be a reserved keyword

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

Whitespace is **required** between two adjacent tokens when both tokens consist entirely of alphanumeric characters (letters `a`–`z` or digits `0`–`9`). Without whitespace, the lexer applies longest-match and produces a single token, not two.

**Normative rule:** If token A ends with an alphanumeric character and token B begins with an alphanumeric character, at least one whitespace character shall appear between them. Omitting this whitespace does not produce a syntax error at the token boundary; instead, A and B merge into a single token under longest-match, which is then classified as an identifier or rejected by the parser as an unexpected token.

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

A token that begins with a keyword string but continues with additional alphanumeric characters is classified as an **identifier**, not as a keyword followed by additional characters. The lexer does not split an alphanumeric run at keyword boundaries.

**Normative rule:** An identifier whose spelling begins with a reserved keyword prefix is a valid identifier, not an error. The keyword is recognised only when it appears as a complete token — that is, when it is followed by a non-alphanumeric character or end of input.

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
Expr            = OrExpr ;
OrExpr          = AndExpr ( '||' AndExpr )* ;
AndExpr         = MatchExpr ( '&&' MatchExpr )* ;
MatchExpr       = CompareExpr ( '|' '{' MatchArmList '}' )? ;
CompareExpr     = AddExpr ( ( '<' | '>' | '=' ) AddExpr )? ;
AddExpr         = MulExpr ( ( '+' | '-' ) MulExpr )* ;
MulExpr         = UnaryExpr ( ( '*' | '/' ) UnaryExpr )* ;
UnaryExpr       = '-' UnaryExpr
                | '!' UnaryExpr
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
                ;

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

The following 8 identifiers are reserved and may not be used as user-defined identifiers:

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
  r=db.one("SELECT id,name FROM users WHERE id=?";@(id))!$usererr.DbErr;
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

Both operands must be of type `bool`. These operators are lower precedence than comparison operators and higher precedence than match expressions.

**Example:**
```
if(a>0 && b>0){
  <a+b
};
```

### 11.12 Match Expression

```
expr|{
  $variant1:binding1 result_expr;
  $variant2:binding2 result_expr
}
```

The match expression applies to a sum type or to a `$result` type. Each arm names a `$`-prefixed variant, binds its payload to a local name, and provides a result expression.

Match is **exhaustive**: the compiler rejects any match that does not cover all variants (E4010). When a variant is added to a sum type, all match expressions on that type fail to compile until they are updated.

The match expression is an expression, not a statement. Its result type is the common type of all arm expressions; all arms must return the same type (E4011).

**Example:**
```
getuser(id)|{
  $ok:u   <$res.ok(json.enc(u));
  $err:e  <$res.err(json.enc(e))
}
```

### 11.13 Error Propagation

```
expr!$errorvariant
```

The `!` operator propagates an error from a partial call. If `expr` evaluates to `Err(e)`, the current function returns `Err($errorvariant(e))`. If `expr` evaluates to `Ok(v)`, execution continues with value `v`.

The right-hand side of `!` must be a variant constructor of the current function's declared error type (E3020).

**Example:**
```
f=handle(req:$http.req):$http.res!$apierr{
  body=json.dec(req.body)!$apierr.BadRequest;
  user=db.getuser(body.id)!$apierr.DbError;
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
  ConnectionFailed:str;
  QueryFailed:str;
  NotFound:u64;
  Timeout:u32
};
```

### 13.3 Error Propagation Rules

The `!` operator: `expr!$targetvariant`

1. Evaluates `expr`
2. If `Ok(v)`: returns `v`, execution continues
3. If `Err(e)`: constructs `$targetvariant(e)` and returns `Err($targetvariant(e))` from the current function

The type of `e` must be coercible to the payload type of `$targetvariant`. The type of `$targetvariant` must be a variant of the current function's declared error type.

If the error types are not coercible, the compiler emits E3021.

### 13.4 Explicit Match on Errors

When propagation is not appropriate, errors are matched explicitly:

```
result|{
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

The standard library modules are part of the toke specification. Their signatures are normative; their implementations are not.

### 16.1 std.str

```
f=str.len(s:str):u64
f=str.concat(a:str;b:str):str
f=str.slice(s:str;start:u64;end:u64):str!$str.sliceerr
f=str.fromint(n:i64):str
f=str.fromfloat(n:f64):str
f=str.toint(s:str):i64!$str.parseerr
f=str.tofloat(s:str):f64!$str.parseerr
f=str.contains(s:str;sub:str):bool
f=str.split(s:str;sep:str):@str
f=str.trim(s:str):str
f=str.upper(s:str):str
f=str.lower(s:str):str
f=str.bytes(s:str):@byte
f=str.frombytes(b:@byte):str!$str.encodingerr
```

### 16.2 std.http

```
t=$req{method:str;path:str;headers:@(@str);body:str;params:@(@str)}
t=$res{status:u16;headers:@(@str);body:str}
t=$httperr{BadRequest:str;NotFound:str;Internal:str;Timeout:u32}

f=http.param(req:$req;name:str):str!$httperr
f=http.header(req:$req;name:str):str!$httperr
f=http.res.ok(body:str):$res
f=http.res.json(status:u16;body:str):$res
f=http.res.bad(msg:str):$res
f=http.res.err(msg:str):$res

(* Route registration — these are compiler-expanded, not runtime calls *)
http.get(pattern:str;handler:f)
http.post(pattern:str;handler:f)
http.put(pattern:str;handler:f)
http.delete(pattern:str;handler:f)
```

### 16.3 std.db

```
t=$row{cols:@(@str)}
t=$dberr{Connection:str;Query:str;NotFound:str;Constraint:str}

f=db.one(sql:str;params:@str):$row!$dberr
f=db.many(sql:str;params:@str):@$row!$dberr
f=db.exec(sql:str;params:@str):u64!$dberr
f=row.str(r:$row;col:str):str!$dberr
f=row.u64(r:$row;col:str):u64!$dberr
f=row.i64(r:$row;col:str):i64!$dberr
f=row.f64(r:$row;col:str):f64!$dberr
f=row.bool(r:$row;col:str):bool!$dberr
```

### 16.4 std.json

```
t=$json{raw:str}
t=$jsonerr{Parse:str;Type:str;Missing:str}

f=json.enc(v:$t):str           (* generic — deferred; uses str for v0.1 *)
f=json.dec(s:str):$json!$jsonerr
f=json.str(j:$json;key:str):str!$jsonerr
f=json.u64(j:$json;key:str):u64!$jsonerr
f=json.i64(j:$json;key:str):i64!$jsonerr
f=json.f64(j:$json;key:str):f64!$jsonerr
f=json.bool(j:$json;key:str):bool!$jsonerr
f=json.arr(j:$json;key:str):@$json!$jsonerr
```

### 16.5 std.file

```
t=$fileerr{NotFound:str;Permission:str;IO:str}

f=file.read(path:str):str!$fileerr
f=file.write(path:str;content:str):bool!$fileerr
f=file.append(path:str;content:str):bool!$fileerr
f=file.exists(path:str):bool
f=file.delete(path:str):bool!$fileerr
f=file.list(dir:str):@str!$fileerr
```

### 16.6 std.net

```
t=$socket{fd:u64}
t=$neterr{Connect:str;Read:str;Write:str;Timeout:u32}

f=net.connect(host:str;port:u16):$socket!$neterr
f=net.read(s:$socket;buf:@byte):u64!$neterr
f=net.write(s:$socket;data:@byte):u64!$neterr
f=net.close(s:$socket):bool
```

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
- wrong variant name: misspelled variant -> `fix: CorrectVariantName`
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
    "11:   r=db.one(sql;@(id))!$usererr.DbErr;",
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

## 24. Deferred Items [N]

The following items are explicitly deferred from version 0.1. Each requires a dedicated follow-on specification or incorporation into version 0.2.

### 24.1 Concurrency and Async Semantics

Async/await, goroutine-style lightweight threads, message passing, channels, and structured concurrency are all deferred. Version 0.1 is single-threaded. The concurrency model must be chosen consistently with the arena memory model and specified in version 0.2.

### 24.2 Foreign Function Interface

The rules for calling C functions from toke, passing raw pointers, managing non-arena memory in FFI contexts, and ABI conventions for each target platform are deferred. A minimal `extern` declaration syntax is reserved but not specified.

### 24.3 Package Registry Governance

Package naming conventions, version resolution, dependency manifest format, registry protocol, and governance structure are deferred.

### 24.4 Formal Memory Model with Ownership Annotations

The arena model is informally specified in Section 14. A formal operational semantics with proof obligations for memory safety is deferred.

### 24.5 Debugger Metadata

DWARF or equivalent metadata for interactive debugging, source maps for IDE integration, and variable inspection are deferred.

### 24.6 Canonical Binary IR

A typed binary intermediate representation for toke programmes suitable for distribution, static analysis, and machine-to-machine exchange without recompilation is deferred.

### 24.7 Tokenizer Vocabulary

The formal vocabulary specification for the purpose-built 32,768-token BPE tokenizer is deferred pending completion of the corpus. The tokenizer specification will form a separate document.

### 24.8 Closures and Higher-Order Functions

First-class functions as values and closures capturing bindings from enclosing scopes are deferred due to interactions with the arena model (captured mutable bindings and arena lifetime).

### 24.9 Generics and Parametric Types

Generic type parameters beyond built-in collection types are deferred to version 0.2.

### 24.10 Option Type

A built-in `$option` type is deferred. In version 0.1, optionality is expressed through sum types: `t=$maybeuser{Some:$user;None:bool}`.

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

### Reserved Keywords

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

### Operators

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
| `\|` | match block open | union type separator |
| `&&` | logical and | -- |
| `\|\|` | logical or | -- |
| `$` | type name sigil prefix | -- |
| `@` | array/map literal and type sigil | -- |
| `^` | reserved, unassigned | -- |
| `~` | reserved, unassigned | -- |

> **Note:** The double-quote `"` appears in source as the string delimiter but is not a structural symbol — it is consumed by the lexer during string literal scanning and never produces a token. See Section 7 note.

---

## Appendix E — Reserved Identifiers [N]

The following identifiers are reserved and may not be used as user-defined names:

**Reserved keywords (Section 11.2):** `if` `el` `lp` `br` `let` `mut` `as` `rt`

**Predefined values:** `true` `false`

**Built-in types (scalar):** `u8` `u16` `u32` `u64` `i8` `i16` `i32` `i64` `f32` `f64` `bool` `str` `byte`

**Built-in type constructors:** `Ok` `Err` (variants of the built-in `$result` type)

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

All other keywords (`if`, `el`, `lp`, `br`, `let`, `mut`, `as`, `rt`) are identical in both modes.

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
  r=db.one("SELECT id,name FROM users WHERE id=?";@(id))!$usererr.DbErr;
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
