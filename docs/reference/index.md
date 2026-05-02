---
title: Language Reference
slug: index
section: reference
order: 7
---

toke is a statically typed, compiled language with a 55-character set, explicit error handling, and a strict declaration order. This page is a structured overview of the language. Each section links to a dedicated reference page.

## Module System

Every toke source file is a module. A file begins with a mandatory module declaration, followed in strict order by imports, type declarations, constants, and functions. Modules expose a typed interface (`.tki` file) for use by other modules.

See [Module System](/docs/reference/modules/) for `m=`, `i=`, stdlib module paths, and interface files.

```toke
m=myapp;
i=io:std.file;
i=log:std.log;
```

## Type System

toke uses a static type system with no implicit coercions. Every value has a single concrete type known at compile time.

**Primitive types:** `i8` `i16` `i32` `i64` `u8` `u16` `u32` `u64` `f32` `f64` `bool` `$str` `void`

**Composite types:**
- Arrays: `@T` — ordered sequence, all elements same type
- Maps: `@($str:T)` — key-value collection
- Structs: `$name` — named product type declared with `t=`
- Sum types: `$name{variant1:T1;variant2:T2;}` — tagged union
- Error unions: `T!$errtype` — success value or error

See [Type System](/docs/reference/types/) for full details, inference rules, and cast semantics.

```toke
t=$point{x:i64;y:i64};
t=$result{$ok:i64;$err:$str};
```

## Functions

Functions are declared with `f=`. Parameters are separated by semicolons. The return type follows the parameter list.

```toke
f=add(a:i64;b:i64):i64{
  < a+b
};
```

- Parameters: `name:type` separated by `;`
- Return type is required (except `void`)
- The `<` operator returns a value — there is no `return` keyword
- A function without a body is an extern (FFI) declaration

See [Statements](/docs/reference/statements/) for the `<` return operator and all statement forms.

## Imports

The `i=` declaration binds a local alias to a module path.

```toke
i=math:std.math;
i=http:std.http;
```

After import, the alias is used to call module functions: `math.sqrt(x)`.

See [Module System](/docs/reference/modules/) for full import syntax and stdlib paths.

## Expressions

Expressions produce values. toke supports:

- Arithmetic: `+` `-` `*` `/`
- Comparison: `=` (equality), `<` `>`, negation with `!(a=b)`, `!(a>b)` for <=, `!(a<b)` for >=
- Boolean: `&&` (and), `||` (or), `!expr` (not)
- String concatenation: `str.concat(a;b)`
- Function calls: `f(arg1;arg2)`
- Field access: `expr.field`
- Cast: `expr as T`
- Error match: `mt result {$ok:v body;$err:e body}`
- Error propagation: `expr!$errtype`

See [Expressions](/docs/reference/expressions/) for syntax and one worked example per operator.

## Statements

Statements appear inside function bodies, separated by semicolons.

| Statement | Syntax |
|-----------|--------|
| Immutable binding | `let x=value;` |
| Mutable binding | `let x=mut.value;` |
| Reassignment | `x=newvalue;` |
| Return | `< expr` |
| Conditional | `if(cond){body}el{body}` |
| Loop | `lp(let i=0;cond;step){body}` |
| Match | `mt result {$variant:v body;...}` |
| Arena block | `{arena stmts}` |

See [Statements](/docs/reference/statements/) for full syntax, semantics, and worked examples.

## Error Handling

toke makes errors explicit via error union types. A function that can fail returns `T!$errtype`. The caller must either handle the error or propagate it.

```toke
m=ref;
i=file:std.file;
t=$err{code:i64};
f=readfile(path:$str):$str!$err{
  let data=file.read(path)!$err;
  < data
};
```

Error match uses the `mt` keyword:

```toke
m=ref;
i=file:std.file;
t=$err{code:i64};
f=readfile(path:$str):$str!$err{
  let data=file.read(path)!$err;
  < data
};
f=demo():$str{
  let result=readfile("data.txt");
  < mt result {
    $ok:data data;
    $err:e ""
  }
};
```

See [Error Handling](/docs/reference/error-handling/) for error union types, match, and propagation.

## The `<` Return Operator

`<` is the return operator. It terminates the current function and produces a value for the caller.

```toke
f=max(a:i64;b:i64):i64{
  if(a>b){< a}el{< b}
};
```

There is no `return` keyword. `<` is always followed by an expression. It can appear multiple times in a function body (early return).

## Identifiers

Identifiers consist of lowercase ASCII letters and digits. They must start with a letter. Underscores are not permitted. Identifiers are case-sensitive. The reserved literals `true` and `false` cannot be used as identifiers.

## Primitives Quick Reference

| Type | Description | Example Literal |
|------|-------------|-----------------|
| `i8` | Signed 8-bit integer | `42` |
| `i16` | Signed 16-bit integer | `1000` |
| `i32` | Signed 32-bit integer | `70000` |
| `i64` | Signed 64-bit integer | `9999999` |
| `u8` | Unsigned 8-bit integer | `255 as u8` |
| `u16` | Unsigned 16-bit integer | `60000 as u16` |
| `u32` | Unsigned 32-bit integer | `0 as u32` |
| `u64` | Unsigned 64-bit integer | `0 as u64` |
| `f32` | 32-bit float | `3.14 as f32` |
| `f64` | 64-bit float | `3.14` |
| `bool` | Boolean | `true` `false` |
| `$str` | Immutable UTF-8 string | `"hello"` |
| `void` | Unit type | (no literal) |
