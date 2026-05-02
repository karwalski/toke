---
title: Type System
slug: types
section: reference
order: 14
---

toke uses a static type system with no implicit coercions. Every value has a single concrete type known at compile time. This page documents all types, inference rules, and compatibility semantics.

## Primitive Types

| Type   | Size    | Range / Values                                          | Zero Value | Literal Examples         |
|--------|---------|---------------------------------------------------------|------------|--------------------------|
| `i8`   | 8 bits  | -128 to 127                                             | `0`        | `42`, `-1`, `0`          |
| `i16`  | 16 bits | -32,768 to 32,767                                       | `0`        | `42`, `-1`, `0`          |
| `i32`  | 32 bits | -2,147,483,648 to 2,147,483,647                         | `0`        | `42`, `-1`, `0`          |
| `i64`  | 64 bits | -9,223,372,036,854,775,808 to 9,223,372,036,854,775,807 | `0`        | `42`, `-1`, `0`          |
| `u8`   | 8 bits  | 0 to 255                                                | `0`        | `0` (via cast from i64)  |
| `u16`  | 16 bits | 0 to 65,535                                             | `0`        | `0` (via cast from i64)  |
| `u32`  | 32 bits | 0 to 4,294,967,295                                      | `0`        | `0` (via cast from i64)  |
| `u64`  | 64 bits | 0 to 18,446,744,073,709,551,615                         | `0`        | `0` (via cast from i64)  |
| `f32`  | 32 bits | IEEE 754 single-precision floating point                 | `0.0`      | `3.14`, `0.0`, `-2.5`   |
| `f64`  | 64 bits | IEEE 754 double-precision floating point                 | `0.0`      | `3.14`, `0.0`, `-2.5`   |
| `bool` | 1 byte  | `true` or `false`                                       | `false`    | `true`, `false`          |
| `$str` | pointer | Immutable UTF-8 string of arbitrary length              | `""`       | `"hello"`, `""`          |
| `void` | 0 bytes | Unit type — no value                                    | n/a        | (no literal form)        |

### Named types and the `$` sigil

Named types — including user-defined structs and the built-in `$str` — are always written with a `$` prefix: `$mytype`, `$str`, `$err`. The `$` sigil distinguishes a type reference from an ordinary identifier and makes type positions unambiguous in the grammar. There are no bare named types without `$`.

### Numeric types

A type is *numeric* if it is `i64`, `u64`, or `f64`. Arithmetic operators require both operands to be the same numeric type. There is no implicit widening or narrowing between numeric types — use `as` to cast explicitly.

```toke
m=types;
f=demo():void{
  let x=42;
  let y=x as f64;
  let z=100 as u64
};
```

### String type

`$str` is an immutable UTF-8 string. String interpolation is not yet supported — use `str.concat()` from the standard library instead.

```toke
m=types;
i=str:std.str;
f=demo():void{
  let name="toke";
  let greeting=str.concat("hello, "; name)
};
```

### Boolean type

`bool` has exactly two values: `true` and `false`. These are reserved literals and cannot be used as identifiers.

## Composite Types

### Arrays — `@T`

An ordered, homogeneous sequence of elements of type `T`.

```toke
m=types;
f=demo():void{
  let nums=@(1;2;3);
  let inferred=@(1;2;3)
};
```

- Element type is determined by the first element, or by annotation.
- An empty array literal `@()` has element type `unknown` until constrained by a type annotation.
- Access `.len` to get the number of elements as `u64`.
- Access an element with `.get(n)` — subscript notation `arr[n]` is not valid in toke.

### Maps — `@($str:V)`

A key-value collection where keys are always `$str` and all values have type `V`. Map type notation is `@($str:T)`. Map literals use `@(key:val; ...)` syntax.

```toke
m=types;
f=demo():void{
  let ages=@("alice":30;"bob":25)
};
```

- Keys are always `$str`. The type `@($str:T)` is the canonical map type form.
- Value type is determined by the first entry, or by annotation.
- All subsequent entries must match. Inconsistent types produce error [E4043](/docs/reference/errors/#e4043).
- Access `.len` to get the number of entries as `u64`.
- Access a value with `.get(key)` — subscript notation `map[key]` is not valid in toke.

### Error Unions — `T!Err`

A sum type representing either a success value of type `T` or an error.

```toke
m=types;
i=file:std.file;
t=$err{code:i64};
f=readfile(path:$str):$str!$err{
  let content=file.read(path)!$err;
  < content
};
```

- The `!` postfix operator unwraps the success value or propagates the error to the caller.
- The enclosing function must also return an error union type to use `!`.
- Applying `!` to a non-error-union value produces error [E3020](/docs/reference/errors/#e3020).

## Special Types

### Pointers — `*T` (FFI only)

Raw pointer types for foreign function interface declarations.

```toke
f=malloc(size: u64): *u8;
f=free(ptr: *u8): void;
```

- Pointer types are **only** valid in extern (bodyless) function signatures.
- Using `*T` in a function with a body produces error [E2010](/docs/reference/errors/#e2010).
- The restriction is recursive: `@*T` and `*@T` are also rejected in non-extern context.

### Function Types — `func`

Function declarations have type `func` internally. Functions are first-class values.

### Struct Types

Named product types declared with `t=`.

```toke
m=types;
t=$point{x:i64;y:i64};
f=origin():$point{
  < $point{x:0;y:0}
};
```

- Two struct types are equal only if they have the same name (nominal typing).
- Field access uses dot notation: `p.x`.
- Accessing a non-existent field produces error [E4025](/docs/reference/errors/#e4025).

### Sum Types (Tagged Unions)

A sum type (tagged union) is declared with `t=` using `$`-prefixed lowercase tag names. Each tag holds a value of the specified type.

```toke
m=types;
t=$shape{$circle:f64;$rect:f64};
f=area(s:$shape):f64{
  < mt s {
    $circle:r r*r*3.14;
    $rect:side side*side
  }
};
```

- Tags use the `$` sigil prefix (e.g. `$circle`, `$rect`). Fields on a plain struct are lowercase without `$`.
- A sum type is matched exhaustively with the `| { ... }` match expression.
- Each arm binds the tag's payload to the given identifier.

## Type Inference

toke infers types for `let` and `mut` bindings when no annotation is provided.

### Inference rules

| Expression Form             | Inferred Type | Notes                          |
|-----------------------------|---------------|--------------------------------|
| Integer literal (`42`)      | `i64`         |                                |
| Floating-point literal (`3.14`) | `f64`     |                                |
| String literal (`"hello"`)  | `$str`        |                                |
| Boolean literal (`true`)    | `bool`        |                                |
| Array literal `@(e1; e2)`   | `@T`          | T = type of first element      |
| Empty array literal `@()`   | `@unknown`    | Requires annotation            |
| Map literal `@(k1:v1; k2:v2)` | `@($str:V)` | key type always `$str`        |
| Struct literal `$name{...}` | `$name`       | Resolved via type declaration  |
| Identifier reference        | declared type | From binding, parameter, or function |
| Call expression `f(args)`   | return type   | From function declaration      |
| Field access `expr.field`   | field type    | From struct or `.len` for collections |
| Cast `expr as T`            | `T`           | Target type                    |

### Binding inference

```toke
m=types;
f=demo():void{
  let x=42;
  let y=3.14;
  let z=mut."hello"
};
```

When both annotation and initializer are present, their types must match. A mismatch produces error [E4031](/docs/reference/errors/#e4031).

## Cast Expressions

The `as` keyword performs explicit type conversion.

```toke
m=types;
f=demo():void{
  let x=42;
  let y=x as f64;
  let z=x as u64
};
```

All casts are unconditionally allowed. A future release may restrict which type pairs are castable.

## Type Compatibility

### Equality rules

Two types are equal according to these rules:

| Rule | Condition |
|------|-----------|
| Identity | A type is equal to itself |
| Primitives | Same kind (`i64 == i64`, but `i64 != u64`) |
| Structs | Same name (nominal equality) |
| Pointers | `*T == *U` iff `T == U` |
| Arrays | `@T == @U` iff `T == U` (or either is `unknown`) |
| Maps | `@($str:V1) == @($str:V2)` iff `V1 == V2` (or either is `unknown`) |
| Error unions | `T1!E1 == T2!E2` iff `T1 == T2` (or either is `unknown`) |
| Functions | Equal iff return types are equal |

### No implicit coercions

toke defines **no implicit coercions**. There is no automatic widening (e.g., `i64` to `f64`), no automatic narrowing, and no implicit conversion between any types. All conversions require an explicit `as` cast.

### The `unknown` type

The internal type `unknown` is a sentinel used when a sub-expression has already emitted an error. It is compatible with any type to suppress cascading errors. It never appears in well-typed programs.
