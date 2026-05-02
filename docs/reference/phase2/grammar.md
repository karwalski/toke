---
title: Default Syntax — Grammar Encoding
slug: phase2-grammar
section: reference
order: 22
---

# Grammar Encoding

This page documents the grammar differences between the default syntax and the [legacy profile](/docs/reference/grammar/). Only changed productions are listed — all other rules are identical.

## Changed Productions

### Type Expressions

The legacy profile uses uppercase-initial identifiers and brackets for composite types. The default syntax uses `$` prefix and `@` sigil.

```ebnf
(* Legacy *)
TypeExpr    = ScalarType | ArrayType | MapType | PtrType | TypeIdent ;
ArrayType   = "[" , TypeExpr , "]" ;
MapType     = "[" , TypeExpr , ":" , TypeExpr , "]" ;
TypeIdent   = UpperIdent ;

(* Default *)
TypeExpr    = ScalarType | ArrayType | MapType | PtrType | SigilType ;
ArrayType   = "@" , TypeExpr ;
MapType     = "@" , "(" , TypeExpr , ":" , TypeExpr , ")" ;
SigilType   = "$" , LowerIdent ;
```

**Examples:**

| Concept | Legacy | Default |
|---------|---------|---------|
| String type | `Str` | `$str` |
| User-defined type | `User` | `$user` |
| Array of integers | `[i64]` | `@i64` |
| Array of strings | `[Str]` | `@$str` |
| Map | `[Str:i64]` | `@($str:i64)` |
| Nested array | `[[i64]]` | `@@i64` |

### Array Literals

```ebnf
ArrayLit    = "[" , [ Expr , { ";" , Expr } ] , "]" ;

ArrayLit    = "@" , "(" , [ Expr , { ";" , Expr } ] , ")" ;
```

| Legacy | Default |
|---------|---------|
| `[1;2;3]` | `@(1;2;3)` |
| `["a";"b"]` | `@("a";"b")` |
| `[]` | `@()` |

### Map Literals

```ebnf
(* Legacy *)
MapLit      = "[" , MapEntry , { ";" , MapEntry } , "]" ;

(* Default *)
MapLit      = "@" , "(" , MapEntry , { ";" , MapEntry } , ")" ;
```

| Legacy | Default |
|---------|---------|
| `["a":1;"b":2]` | `@("a":1;"b":2)` |

### Array Indexing

```ebnf
IndexExpr   = PostfixExpr , "[" , Expr , "]" ;

IndexExpr   = PostfixExpr , "." , "get" , "(" , Expr , ")" ;
```

| Legacy | Default |
|---------|---------|
| `arr[0]` | `arr.get(0)` |
| `arr[i]` | `arr.get(i)` |
| `arr[i+1]` | `arr.get(i+1)` |

### Identifiers

```ebnf
(* Legacy *)
Ident       = Letter , { Letter | Digit } ;
Letter      = "a".."z" | "A".."Z" ;
TypeIdent   = UpperLetter , { Letter | Digit } ;

(* Default *)
Ident       = LowerLetter , { LowerLetter | Digit } ;
LowerLetter = "a".."z" ;
SigilIdent  = "$" , LowerLetter , { LowerLetter | Digit } ;
```

There is no `TypeIdent` in the default syntax. All type references use `SigilIdent` (`$name`).

### Struct Literals

```ebnf
StructLit   = TypeIdent , "{" , FieldInit , { ";" , FieldInit } , "}" ;

StructLit   = SigilIdent , "{" , FieldInit , { ";" , FieldInit } , "}" ;
```

| Legacy | Default |
|---------|---------|
| `Point{x:1;y:2}` | `$point{x:1;y:2}` |
| `User{id:1;name:"alice"}` | `$user{id:1;name:"alice"}` |

### Match Arms

```ebnf
(* Legacy *)
MatchArm    = TypeIdent , ":" , Ident , Expr ;

(* Default *)
MatchArm    = SigilIdent , ":" , Ident , Expr ;
```