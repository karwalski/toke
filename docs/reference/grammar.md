---
title: Grammar
slug: grammar
section: reference
order: 6
---

This page defines the formal grammar of toke. The grammar is LL(1)-compatible, meaning it can be parsed with a single token of lookahead and no backtracking.

## EBNF Notation

The grammar uses standard EBNF notation:

| Notation    | Meaning                           |
|-------------|-----------------------------------|
| `=`         | Definition                        |
| `,`         | Concatenation                     |
| `\|`        | Alternation                       |
| `[ ... ]`   | Optional (zero or one)            |
| `{ ... }`   | Repetition (zero or more)         |
| `( ... )`   | Grouping                          |
| `"..."`     | Terminal string                   |
| `(*...*)`   | Comment                           |

## Production Rules

### Module Structure

```ebnf
Module      = ModuleDecl , { ImportDecl } , { TypeDecl } , { ConstDecl } , { FuncDecl } ;

ModuleDecl  = "m" , "=" , Ident , ";" ;

ImportDecl  = "i" , "=" , Ident , ":" , ModulePath , [ VersionStr ] , ";" ;

ModulePath  = Ident , { "." , Ident } ;

VersionStr  = StringLit ;
```

Declarations must appear in this exact order: module, imports, types, constants, functions. Violating this order produces error [E2001](/docs/reference/errors/#e2001).

### Type Declarations

```ebnf
TypeDecl    = "t" , "=" , "$" , Ident , "{" , FieldList , "}" , ";" ;

FieldList   = Field , { ";" , Field } , [ ";" ] ;

Field       = Ident , ":" , TypeExpr ;
```

### Constant Declarations

```ebnf
ConstDecl   = "c" , "=" , Ident , [ ":" , TypeExpr ] , Expr , ";" ;
```

### Function Declarations

```ebnf
FuncDecl    = "f" , "=" , Ident , "(" , [ ParamList ] , ")" ,
              [ ":" , TypeExpr ] , ( Block | ";" ) ;

ParamList   = Param , { ";" , Param } ;

Param       = Ident , ":" , TypeExpr ;

Block       = "{" , StmtList , "}" ;
```

A function declaration without a body (terminated by `;` instead of a block) is an **extern declaration** for FFI.

### Type Expressions

```ebnf
TypeExpr    = PrimType
            | "@" , TypeExpr                               (* array type: @T *)
            | "@" , "(" , TypeExpr , ":" , TypeExpr , ")"  (* map type: @(K:V) *)
            | TypeExpr , "!" , TypeExpr                    (* error union: T!$err *)
            | "*" , TypeExpr                               (* pointer, FFI only *)
            | "$" , Ident                                  (* named type / struct *)
            ;

PrimType    = "i8" | "i16" | "i32" | "i64"
            | "u8" | "u16" | "u32" | "u64"
            | "f32" | "f64"
            | "bool" | "$str" | "void"
            ;
```

### Statements

```ebnf
StmtList    = { Stmt , [ ";" ] } ;

Stmt        = LetStmt
            | MutLetStmt
            | AssignStmt
            | ReturnStmt
            | IfStmt
            | MatchExpr
            | LoopStmt
            | ArenaStmt
            | ExprStmt
            ;

LetStmt     = "let" , Ident , [ ":" , TypeExpr ] , "=" , Expr ;

MutLetStmt  = "let" , Ident , [ ":" , TypeExpr ] , "=" , "mut" , "." , Expr ;

AssignStmt  = Ident , "=" , Expr ;

ReturnStmt  = "<" , Expr ;

IfStmt      = "if" , "(" , Expr , ")" , Block , [ "el" , Block ] ;

LoopStmt    = "lp" , "(" , [ LoopInit ] , ";" , [ Expr ] , ";" , [ Expr ] , ")" , Block ;

LoopInit    = "let" , Ident , [ ":" , TypeExpr ] , "=" , Expr ;

ArenaStmt   = "{" , "arena" , StmtList , "}" ;

ExprStmt    = Expr ;
```

### Expressions

```ebnf
Expr        = UnaryExpr , [ BinOp , Expr ]
            | CallExpr
            | FieldExpr
            | CastExpr
            | PropagateExpr
            | MatchExpr
            | Literal
            | Ident
            | "(" , Expr , ")"
            ;

UnaryExpr   = [ "-" ] , PrimaryExpr ;

BinOp       = "+" | "-" | "*" | "/" | "<" | ">" | "=" | "&&" | "||" ;

CallExpr    = Ident , "(" , [ ArgList ] , ")" ;

ArgList     = Expr , { ";" , Expr } ;

FieldExpr   = Expr , "." , Ident ;

CastExpr    = Expr , "as" , TypeExpr ;

PropagateExpr = Expr , "!" , TypeExpr ;

MatchExpr   = Expr , "|" , "{" , MatchArm , { ";" , MatchArm } , [ ";" ] , "}" ;

MatchArm    = TypeIdent , ":" , Ident , Expr ;
```

### Literals

```ebnf
IntLit      = Digit , { Digit } ;

FloatLit    = Digit , { Digit } , "." , Digit , { Digit } ;

StringLit   = '"' , { StringChar } , '"' ;

StringChar  =
            | EscapeSeq ;

EscapeSeq   = '\' , ( '"' | '\' | 'n' | 't' | 'r' | '0' | HexEscape ) ;

HexEscape   = 'x' , HexDigit , HexDigit ;

BoolLit     = "true" | "false" ;

ArrayLit    = "@" , "(" , [ Expr , { ";" , Expr } , [ ";" ] ] , ")" ;

MapLit      = "@" , "(" , MapEntry , { ";" , MapEntry } , [ ";" ] , ")" ;

MapEntry    = Expr , ":" , Expr ;

StructLit   = "$" , Ident , "{" , FieldInit , { ";" , FieldInit } , [ ";" ] , "}" ;

FieldInit   = Ident , ":" , Expr ;

Ident       = Letter , { Letter | Digit } ;

Letter      = "a" .. "z" ;

Digit       = "0" .. "9" ;

HexDigit    = Digit | "a" .. "f" | "A" .. "F" ;
```

## Whitespace

Whitespace (space, tab, carriage return, line feed) is not a structural element of toke source. **Whitespace separates tokens but has no other structural role.** The lexer discards whitespace outside string literals after using it to determine token boundaries. Any amount of whitespace between two tokens is equivalent — two programs that differ only in whitespace between tokens are semantically identical.

Whitespace is **required** only between adjacent tokens that both consist entirely of alphanumeric characters; otherwise the longest-match lexer would merge them into a single token.

## Syntax Profile

toke has two syntax profiles. All productions on this page use **default syntax**.

| Feature | Default (55-char) | Legacy (80-char, `--legacy`) |
|---------|-------------------|------------------------------|
| Keywords | `m=` `f=` `t=` `i=` | `M=` `F=` `T=` `I=` |
| Type names | `$user`, `$str` | `User`, `Str` |
| Array literal | `@(1; 2; 3)` | `[1; 2; 3]` |
| Array type | `@i64` | `[i64]` |
| Map type | `@($str:i64)` | `[Str:i64]` |
| Array indexing | `arr.get(0)` | `arr[0]` |
| `$` and `@` | structural symbols | not available |
| `[` and `]` | not available | array delimiters |

The compiler rejects `[` and `]` in default mode (E1003) and rejects `$` and `@` in legacy mode (E1003). The compiler currently accepts both uppercase and lowercase declaration keywords in default mode; the spec reserves uppercase keywords for legacy mode only.

See spec Appendix F for full legacy profile documentation.

## Reading the Grammar

Each EBNF production describes how a syntactic construct is formed from tokens. Here is how to read them:

**Example:** `LetStmt = "let" , Ident , [ ":" , TypeExpr ] , "=" , Expr ;`

This says: a let statement is the keyword `let`, followed by an identifier, optionally followed by a colon and type expression, then `=`, then an expression.

In toke source, that looks like:

```toke
m=ex;
f=main():void{
  let x=42;
  let y:f64=3.14
};
```

**Example:** `MatchExpr = "mt" , Expr , "{" , MatchArm , { ";" , MatchArm } , "}" ;`

This says: a match expression is `mt`, then an expression, then `{`, then one or more match arms separated by `;`, then `}`.

```toke
m=ex;
f=check(r:i64!$str):i64{
  <mt r {$ok:v v;$err:e 0}
};
```

The formal EBNF is in `spec/grammar.ebnf`. The normative grammar uses legacy-profile keywords (`M=`/`F=`/`T=`/`I=`); the default-syntax equivalents are lowercase.

## Key Productions Explained

### Module

Every toke source file is a `Module`. It begins with a mandatory module declaration (`m=name;`) followed by optional imports, type declarations, constants, and function declarations -- in that strict order.

```toke
m=myapp;
i=io:std.file;
t=$config{port:i64;host:$str};
f=main(): void { };
```

### FuncDecl

Functions are declared with `f=name(params):ReturnType { body }`. A function without a body is an extern (FFI) declaration. The return statement uses `<` instead of a `return` keyword.

```toke
f=add(a: i64; b: i64): i64 { < a + b };
f=puts(s: *u8): void;
```

### TypeExpr

Type expressions describe the type of a value. They can be primitives, arrays (`@T`), maps (`@(K:V)`), error unions (`T!$err`), pointers (`*T`, FFI only), or named struct types (`$name`).

### Stmt

Statements within a block are separated by semicolons. The trailing semicolon before a closing `}` or at end-of-file may be omitted (trailing-semicolon elision).

### Expr

Expressions produce values. The `<` return statement, `match` expressions, `as` casts, and the `!` propagation operator are all expression forms.

## LL(1) Property

The toke grammar is designed to be LL(1)-parseable:

- **Single token lookahead.** At every decision point in the grammar, the parser can determine which production to use by examining only the current token.
- **No backtracking.** The parser never needs to speculatively try a production and then undo work.
- **Predictable performance.** Parsing time is linear in the number of tokens.

Key design choices that enable LL(1) parsing:

- Declaration prefixes (`m=`, `i=`, `t=`, `c=`, `f=`) are unique single-token lookaheads.
- Statement prefixes (`let`, `<`, `lp`, `if`, `{arena`) are distinct.
- The `<` return operator avoids ambiguity with the comparison `<` because return always appears at statement position.

## Operator Precedence

Operators are listed from highest to lowest precedence:

| Precedence | Operator(s)        | Associativity | Description              |
|------------|--------------------|---------------|--------------------------|
| 1 (highest)| `.`                | Left          | Field access             |
| 2          | `()`               | Left          | Function call            |
| 3          | `!` (postfix)      | Left          | Error propagation        |
| 4          | `-` (unary)        | Right         | Negation                 |
| 5          | `as`               | Left          | Type cast                |
| 6          | `*`, `/`           | Left          | Multiplication, division |
| 7          | `+`, `-`           | Left          | Addition, subtraction    |
| 8          | `<`, `>`, `=`      | Left          | Comparison               |
| 9          | `&&`               | Left          | Logical AND              |
| 10 (lowest)| `\|\|`             | Left          | Logical OR               |

### Precedence examples

```toke
m=grammar;
f=examples(a:i64;b:i64;c:i64;x:@i64):void{
  let r1=a+b*c;
  let r2=x.len+1;
  let r4=x.get(0) as f64+1.0;
  let r5=a>0
};
```
