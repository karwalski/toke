# Core Structural Symbol Audit

**Story:** 77.1.1
**Date:** 2026-05-01
**Spec version:** toke-spec-v0.3
**Symbols audited:** `(` `)` `{` `}` `;` `:` `.` `=`

---

## 1. Parentheses `(` `)`

| # | Use | Spec reference | Implemented in compiler | Used in stdlib .tk files |
|---|-----|----------------|------------------------|--------------------------|
| 1 | Function call argument list | §10 `CallExpr = PostfixExpr ('(' ArgList ')')*` | Yes — `parse_call()` in parser.c:536 consumes `TK_LPAREN` | Yes — `os.open(path;os.o_rdonly();0)` etc. |
| 2 | Function declaration parameter list | §10 `FuncDecl = 'f' '=' IDENT '(' ParamList ')'...` | Yes — parser.c handles `TK_LPAREN`/`TK_RPAREN` in func decl | Yes — `f=read(path:$str):$str!$fileerr{` |
| 3 | Grouping / parenthesised sub-expression | §10 `PrimaryExpr = '(' Expr ')'` | Yes — `parse_primary()` parser.c:451 | Yes — `(sz as u64)+1`, `(buf as i64)+total` |
| 4 | Array literal `@(expr; expr; ...)` | §10 `ArrayLit = '@' '(' ... ')'` | Yes — `parse_primary()` parser.c:434-449 | Yes — `@(id)` in file.tk |
| 5 | Map literal `@(key:val; key:val; ...)` | §10 `MapLit = '@' '(' Expr ':' Expr ... ')'` | Yes — `parse_primary()` parser.c:439-445 | Not observed in current .tk stubs |
| 6 | Map type expression `@($key:$val)` | §10 `MapTypeExpr = '@' '(' TypeExpr ':' TypeExpr ')'` | Yes — `parse_type_expr()` parser.c:317-322 | Not observed |
| 7 | Function type expression `(T1;T2):Tret` | §10 `FuncTypeExpr = '(' TypeExpr (';' TypeExpr)* ')' ':' TypeExpr` | Yes — `parse_func_type()` parser.c:272-278 | Not observed |
| 8 | Block comment delimiter `(* ... *)` | §8.10 | Yes — lexer.c:615-632, nested depth tracking | Yes — `(* std.str — stub *)` |
| 9 | Doc comment delimiter `(** ... *)` | §8.10 | Yes — same code path as block comments (starts with `(*`) | Not observed in current stubs |
| 10 | Loop syntax `lp(init;cond;step)` | §10 `LoopStmt = 'lp' '(' Stmt Expr ';' Stmt ')'...` | Yes — `parse_loop_stmt()` parser.c:804-828 | Yes — `lp(let i=0;i<arr.len;i=i+1){` in spec examples |
| 11 | Conditional `if(cond)` | §10 `IfStmt = 'if' '(' Expr ')'...` | Yes — `parse_if_stmt()` parser.c:851-865 | Yes — `if(e==2){` in file.tk |
| 12 | String interpolation `\(expr)` (inside strings) | §8.7 | Partial — lexer.c:431-442 emits W1010 warning, skips via depth tracking | Not used (not supported in Profile 1) |

### Surprising dual-uses or overloads

- **Comment delimiters `(* ... *)`** — the `(` character is overloaded between grouping/calls and comment opening. Disambiguation is purely lexical: `(` followed immediately by `*` triggers comment mode in the lexer before any token is emitted.
- **String interpolation `\(`** — the `(` inside `\(...)` in string literals participates in nested parenthesis depth tracking within the lexer, but this is not a structural use since it is inside a string body.

### Could it be removed?

**No.** Parentheses are fundamental to:
- Unambiguous function call syntax (distinguishing `f` the identifier from `f(x)` the call)
- LL(1) parsing of conditionals and loops (the condition must be delimited)
- Array/map literal syntax `@(...)` (the only collection literal form after `[]` was removed)
- Block comments `(* *)` (no alternative comment syntax exists)

Removing `(` `)` would require introducing multiple new delimiter characters, breaking the 59-character set constraint.

---

## 2. Braces `{` `}`

| # | Use | Spec reference | Implemented in compiler | Used in stdlib .tk files |
|---|-----|----------------|------------------------|--------------------------|
| 1 | Function body | §10 `FuncDecl = ... '{' StmtList '}' ';'` | Yes — parser.c func decl parsing | Yes — `f=makerr(...):$fileerr{...}` |
| 2 | Struct/type declaration body | §10 `TypeDecl = 't' '=' '$' TypeName '{' FieldList '}' ';'` | Yes — parser.c type decl parsing | Yes — `t=$fileerr{$notfound:$str;...}` |
| 3 | Struct literal | §10 `StructLit = '$' IDENT '{' FieldInit (';' FieldInit)* '}'` | Yes — `parse_primary()` parser.c:419-429 | Yes — `$fileerr.$notfound(...)` (call form, not literal in file.tk) |
| 4 | If-statement body | §10 `IfStmt = 'if' '(' Expr ')' '{' StmtList '}'...` | Yes — `parse_if_stmt()` parser.c:857-858 | Yes — `if(e==2){ ... }` |
| 5 | Else body | §10 `... ('el' '{' StmtList '}')?` | Yes — `parse_if_stmt()` parser.c:860-863 | Not observed in stubs |
| 6 | Loop body | §10 `LoopStmt = ... '{' StmtList '}'` | Yes — `parse_loop_stmt()` parser.c:823-825 | Yes — spec examples |
| 7 | Match arms block | §10 `MatchExpr = ... '\|' '{' MatchArmList '}'` | Yes — `parse_expr()` parser.c:764-769 | Not observed in stubs |
| 8 | Arena block | ��10 `ArenaStmt = '{' 'arena' StmtList '}'` | Yes — parser.c stmt dispatch (TK_LBRACE with "arena" lookahead) | Not observed |

### Surprising dual-uses or overloads

- **No overload ambiguity.** All `{` uses are preceded by a distinct syntactic leader: return type (func body), type name (struct decl/literal), `)` (if/lp body), `el` (else), `|` (match). The parser always knows which production applies from context.
- The **arena block** `{arena ...}` is the only case where `{` appears at statement-start without a preceding keyword, but it is disambiguated by the `arena` identifier immediately inside.

### Could it be removed?

**No.** Braces are the sole block-scoping mechanism in toke. Without them:
- Function bodies, loop bodies, and if-branches would have no delimiter
- Struct type field lists would need an alternative syntax
- Match arm grouping would require another delimiter

The grammar would lose its LL(1) property without braces.

---

## 3. Semicolon `;`

| # | Use | Spec reference | Implemented in compiler | Used in stdlib .tk files |
|---|-----|----------------|------------------------|--------------------------|
| 1 | Statement terminator | §10 `BindStmt = 'let' IDENT '=' Expr ';'` etc. | Yes — `opt_semi()` parser.c:207-211 | Yes — every statement in file.tk |
| 2 | Top-level declaration terminator | §10 `FuncDecl = ... '}' ';'` | Yes — parser.c top-level parsing | Yes — `t=$fileerr{...};` |
| 3 | Function parameter separator | §10 `ParamList = Param (';' Param)*` | Yes — parser.c func decl parsing | Yes — `f=read(path:$str):$str!$fileerr{` (single param, but multi-param uses `;`) |
| 4 | Function argument separator (in calls) | §10 `ArgList = Expr (';' Expr)*` | Yes — `parse_call()` parser.c:539 | Yes — `os.open(path;os.o_rdonly();0)` |
| 5 | Struct field separator | §10 `FieldList = Field (';' Field)*` | Yes — parser.c type decl | Yes — `t=$fileerr{$notfound:$str;$permission:$str;$io:$str}` |
| 6 | Struct literal field init separator | §10 `StructLit = '$' IDENT '{' FieldInit (';' FieldInit)* '}'` | Yes — `parse_primary()` parser.c:407, 427 | Not observed in stubs |
| 7 | Array literal element separator | §10 `ArrayLit = '@' '(' Expr (';' Expr)* ')'` | Yes — parser.c:448 | Yes — `@(id)` (single element) |
| 8 | Map literal entry separator | §10 `MapLit = '@' '(' Expr ':' Expr (';' ...) ')'` | Yes — parser.c:442-444 | Not observed |
| 9 | Match arm separator | §10 `MatchArmList = MatchArm (';' MatchArm)*` | Yes — `parse_expr()` parser.c:767 | Not observed |
| 10 | Loop clause separator `lp(init;cond;step)` | §10 `LoopStmt` | Yes — `parse_loop_stmt()` parser.c:814-816 | Spec examples |
| 11 | Function type param separator | §10 `FuncTypeExpr = '(' TypeExpr (';' TypeExpr)* ')' ':' TypeExpr` | Yes — `parse_func_type()` parser.c:275 | Not observed |
| 12 | Import declaration terminator | §10 `ImportDecl = 'i' '=' IDENT ':' ModulePath ';'` | Yes — parser.c | Yes — `i=os:std.os;` |
| 13 | Module declaration terminator | §10 `ModuleDecl = 'm' '=' ModulePath ';'` | Yes — parser.c | Yes — `m=file;` |

### Surprising dual-uses or overloads

- The semicolon serves as **both** a statement terminator **and** a separator (parameters, arguments, fields, match arms, array elements). This is a deliberate design choice: toke uses `;` as the universal delimiter, replacing `,` (which is excluded from the character set).
- **Trailing semicolon elision:** The last item before `}` or EOF may omit its trailing `;` (implemented in `opt_semi()`). This is the only context-sensitivity around `;`.

### Could it be removed?

**No.** Without `;`:
- Statement boundaries would be ambiguous (no whitespace-as-syntax rule exists)
- Function parameters could not be distinguished from a single multi-word expression
- The LL(1) property would be lost — the parser would need unbounded lookahead

The semicolon is the most heavily overloaded symbol in toke, but each use is unambiguous due to parser context.

---

## 4. Colon `:`

| # | Use | Spec reference | Implemented in compiler | Used in stdlib .tk files |
|---|-----|----------------|------------------------|--------------------------|
| 1 | Parameter type annotation | §10 `Param = IDENT ':' TypeExpr` | Yes — parser.c func decl | Yes — `f=makerr(fallback:$str)` |
| 2 | Return type annotation | §10 `FuncDecl = ... ')' ':' ReturnSpec ...` | Yes �� parser.c func decl | Yes — `f=read(path:$str):$str!$fileerr{` |
| 3 | Struct field type | §10 `Field = IDENT ':' TypeExpr` | Yes ��� parser.c type decl | Yes — `t=$fileerr{$notfound:$str;...}` |
| 4 | Struct literal field value | §10 `FieldInit = IDENT ':' Expr` | Yes — `parse_primary()` parser.c:406, 426 | Not observed in stubs |
| 5 | Import alias separator | §10 `ImportDecl = 'i' '=' IDENT ':' ModulePath ';'` | Yes — parser.c import | Yes — `i=os:std.os;` |
| 6 | Map literal key:value separator | §10 `MapLit = '@' '(' Expr ':' Expr ... ')'` | Yes — `parse_primary()` parser.c:439, 444 | Not observed |
| 7 | Map type key:value separator | §10 `MapTypeExpr = '@' '(' TypeExpr ':' TypeExpr ')'` | Yes — `parse_type_expr()` parser.c:319 | Not observed |
| 8 | Match arm binding separator | §10 `MatchArm = TypeExpr ':' IDENT Expr` | Yes — `parse_match_arm()` parser.c:728, 737 | Not observed |
| 9 | Function type return type separator | §10 `FuncTypeExpr = '(' ... ')' ':' TypeExpr` | Yes — `parse_func_type()` parser.c:277 | Not observed |
| 10 | Constant declaration type annotation | §10 `ConstDecl = IDENT '=' LiteralExpr ':' TypeExpr ';'` | Yes — parser.c | Not observed |

### Surprising dual-uses or overloads

- The colon is used for **type annotations** (`:$type`), **field/map key-value separation** (`:val`), and **import aliasing** (`alias:path`). These are disambiguated by position:
  - After an identifier in a parameter/field list = type annotation
  - Between two expressions in `@(...)` = map key:value
  - After an identifier followed by a module path in `i=` = import alias
- No lexical ambiguity exists because the parser always knows which production is active.

### Could it be removed?

**No.** The colon carries essential type information in every function signature, struct definition, and import. Removing it would require:
- A new separator for type annotations (keywords like `as` are already taken)
- A new key-value delimiter for maps
- Breaking the one-character-per-role discipline

---

## 5. Dot `.`

| # | Use | Spec reference | Implemented in compiler | Used in stdlib .tk files |
|---|-----|----------------|------------------------|--------------------------|
| 1 | Member/field access | §10 `PostfixExpr = PrimaryExpr ('.' IDENT)*` | Yes — `parse_postfix()` parser.c:498-509 | Yes — `os.errno()`, `os.o_rdonly()` |
| 2 | Module path separator | §10 `ModulePath = IDENT ('.' IDENT)*` | Yes — parser.c module/import parsing | Yes — `i=os:std.os;` |
| 3 | Method-style calls (chained) | §10 (PostfixExpr + CallExpr) | Yes — postfix + call chain | Yes — `s.concat("...";fallback)` |
| 4 | Mutable qualifier `mut.` | §11.8 `MutBindStmt = 'let' IDENT '=' 'mut' '.' Expr ';'` | Yes — parser.c stmt dispatch | Yes — `let total=mut.0 as i64` |
| 5 | Float literal decimal point | §8.6 | Yes — `lex_number()` lexer.c:356-361 | Yes — implicit in numeric contexts |
| 6 | `.get(i)` indexing syntax | §12.2 (array access) | Yes — `parse_postfix()` parser.c:500-506 special-cases `.get(` | Not observed in stubs |

### Surprising dual-uses or overloads

- **`mut.` prefix** — the dot after `mut` in mutable bindings (`let x=mut.0`) is syntactically a member access token, but semantically it acts as a qualifier separator. This is unusual because `mut` is a keyword, not a value. The parser handles this specially.
- **Float vs. member access** — `42.method` could theoretically be ambiguous, but the lexer resolves this: a dot after digits is only treated as a decimal point if the next character is also a digit (`lexer.c:356-357`). So `42.x` produces `TK_INT_LIT` `TK_DOT` `TK_IDENT`.
- **Module path = member access** — syntactically, `std.io` and `obj.field` are identical (`PostfixExpr`). The semantic distinction is made during name resolution, not parsing.

### Could it be removed?

**No.** The dot is essential for:
- All qualified access (modules, fields, methods) — there is no alternative accessor syntax
- The `mut.` mutable binding form
- Float literals (required by the language)

No other single character could replace its role without adding to the character set.

---

## 6. Equals `=`

| # | Use | Spec reference | Implemented in compiler | Used in stdlib .tk files |
|---|-----|----------------|------------------------|--------------------------|
| 1 | Module declaration `m=path` | §10 `ModuleDecl = 'm' '=' ModulePath ';'` | Yes — parser.c top-level | Yes — `m=file;` |
| 2 | Import declaration `i=alias:path` | §10 `ImportDecl = 'i' '=' IDENT ':' ModulePath ';'` | Yes — parser.c | Yes — `i=os:std.os;` |
| 3 | Type declaration `t=$name{...}` | §10 `TypeDecl = 't' '=' '$' TypeName ...` | Yes — parser.c | Yes — `t=$fileerr{...}` |
| 4 | Function declaration `f=name(...)` | §10 `FuncDecl = 'f' '=' IDENT ...` | Yes — parser.c | Yes — `f=makerr(...)` |
| 5 | Let binding `let x=expr` | §10 `BindStmt = 'let' IDENT '=' Expr ';'` | Yes — parser.c stmt dispatch | Yes — `let e=os.errno()` |
| 6 | Assignment to mutable `x=expr` | §10 `AssignStmt = IDENT '=' Expr ';'` | Yes — parser.c stmt dispatch | Yes — `ok=false` |
| 7 | Loop init `let i=0` | §10 `LoopInit` | Yes — `parse_loop_stmt()` parser.c:812 | Spec examples |
| 8 | Loop step `i=i+1` | §10 `LoopStep` | Yes — `parse_loop_stmt()` parser.c:820 | Spec examples |
| 9 | Equality comparison in expressions | §10 `EqExpr = CompareExpr ('=' CompareExpr)?` | Yes — `parse_compare()` parser.c:661 | Yes — `if(e==2)` ... wait, that's `==` which is not a toke operator. In toke: `if(sz<0)`, `if(buf==0)` — **note:** file.tk uses `==` which may be a bug; spec says single `=` for equality |
| 10 | Constant declaration `name=lit:type` | §10 `ConstDecl = IDENT '=' LiteralExpr ':' TypeExpr ';'` | Yes — parser.c | Not observed |

### Surprising dual-uses or overloads

- **`=` is both binding AND equality comparison.** This is the most significant overload in toke. Disambiguation relies entirely on parser context:
  - At statement level, `IDENT '='` is assignment/binding
  - In expression context (inside `parse_compare`), `=` is equality
  - The grammar is LL(1) because declarations/assignments always start at statement position, while equality only appears between sub-expressions
- **No `==` operator exists** in the spec. A single `=` is both assignment and equality. This is a deliberate token-efficiency choice but requires careful parser design.
- **`file.tk` appears to use `==`** (e.g., `if(buf==0)`). This is either a corpus inconsistency or the lexer treats `==` as `=` `=` (two equality tokens, which would be a parse error). This warrants investigation.

### Could it be removed?

**No.** The equals sign is:
- The only binding operator in the language (no `:=`, `<-`, or other alternatives)
- The only equality comparison operator (no `==`)
- Part of all four declaration keywords (`m=`, `f=`, `t=`, `i=`)

It is arguably the most critical single character in toke's syntax.

---

## Summary Table

| Symbol | Distinct uses | Overloaded? | Removable? | Notes |
|--------|--------------|-------------|------------|-------|
| `(` `)` | 12 | Yes (grouping + calls + comments + collections) | No | Comment syntax `(*` is the most surprising overload |
| `{` `}` | 8 | No real ambiguity (always preceded by distinct leader) | No | Cleanest structural symbol — one role (block scoping) |
| `;` | 13 | Yes (terminator + separator in 6+ contexts) | No | Most heavily loaded; universal delimiter replacing `,` |
| `:` | 10 | Yes (types + key:val + import alias + match binding) | No | All uses mean "associates left with right" |
| `.` | 6 | Yes (member access + module path + mut. + floats) | No | `mut.` is the most surprising overload |
| `=` | 10 | Yes (binding + equality — most significant) | No | Single `=` for both assignment and equality is unique to toke |

---

## Open Questions / Action Items

1. **`==` in file.tk** — The stdlib file `/Users/matthew.watt/tk/toke/stdlib/file.tk` uses `==` for equality (e.g., `if(buf==0)`). The spec says single `=` is equality. Either file.tk is non-conforming or the compiler accepts `==` as a compatibility form. Needs verification.
2. **`loop{` vs `lp()`** — file.tk line 41 uses bare `loop{` instead of `lp(init;cond;step)`. This may be an older form or a simplified infinite-loop variant not in the v0.3 grammar.
3. **Semicolon elision edge cases** — The trailing-semicolon elision rule in `opt_semi()` could interact subtly with match arms and struct literals. Current test coverage should verify these boundaries.
