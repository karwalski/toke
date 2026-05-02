---
title: Expressions
slug: expressions
section: reference
order: 4
---

An expression is any syntactic form that produces a value. Expressions can appear as the right-hand side of a `let` binding, as function arguments, as the operand of `<`, and in most other value positions.

## Arithmetic

Arithmetic operators require both operands to be the same numeric type. There is no implicit widening. Use `as` to cast between numeric types before applying an operator.

### Addition — `+`

```toke
m=exprs;
f=demo():i64{
  let a=10;
  let b=3;
  < a+b
};
```

Result: `13`

### Subtraction — `-`

```toke
m=exprs;
f=demo():i64{
  let total=100;
  let used=37;
  < total-used
};
```

Result: `63`

### Multiplication — `*`

```toke
m=exprs;
f=demo():i64{
  let width=8;
  let height=6;
  < width*height
};
```

Result: `48`

### Division — `/`

Integer division truncates toward zero. Floating-point division follows IEEE 754.

```toke
m=exprs;
f=demo():i64{
  let n=17;
  let d=5;
  < n/d
};
```

Result: `3`

```toke
m=exprs;
f=demof():f64{
  let n=17.0;
  let d=5.0;
  < n/d
};
```

Result: `3.4`

---

## Comparison

Comparison operators return `bool`.

### Equality — `=`

In toke, `=` is both assignment (in binding context) and equality comparison (in expression context). The parser distinguishes them by position.

```toke
m=exprs;
f=demo():bool{
  let x=5;
  < x=5
};
```

Result: `true`

### Inequality — `!(a=b)`

toke has no `!=` operator. Express "not equal" using the logical not operator applied to an equality test.

```toke
m=exprs;
f=demo():bool{
  let x=5;
  let y=7;
  < !(x=y)
};
```

Result: `true`

### Less than — `<` (in expression context)

As a binary operator, `<` compares two values. As a statement prefix, it is the return operator. The parser knows which meaning applies by context: `<` at statement position is return; `<` with a left-hand operand is comparison.

```toke
m=exprs;
f=demo():bool{
  let a=3;
  let b=10;
  < a<b
};
```

Result: `true`

### Greater than — `>`

```toke
m=exprs;
f=demo():bool{
  let score=95;
  < score>80
};
```

Result: `true`

### Less than or equal — `!(a>b)`

toke has no `<=` operator. Express "less than or equal" using logical not applied to greater-than.

```toke
m=exprs;
f=demo():bool{
  let count=10;
  let limit=10;
  < !(count>limit)
};
```

Result: `true`

### Greater than or equal — `!(a<b)`

toke has no `>=` operator. Express "greater than or equal" using logical not applied to less-than.

```toke
m=exprs;
f=demo():bool{
  let level=5;
  < !(level<3)
};
```

Result: `true`

---

## Boolean

Boolean operators work on `bool` operands and return `bool`.

### Logical AND — `&&`

Returns `true` only if both operands are `true`.

```toke
m=exprs;
f=demo():bool{
  let active=true;
  let valid=true;
  < active&&valid
};
```

Result: `true`

### Logical OR — `||`

Returns `true` if either operand is `true`.

```toke
m=exprs;
f=demo():bool{
  let a=false;
  let b=true;
  < a||b
};
```

Result: `true`

### Logical NOT — `!expr`

Negates a `bool` value. When applied to a comparison, `!` must wrap the whole comparison: `!(a=b)`.

```toke
m=exprs;
f=demo():bool{
  let enabled=false;
  < !enabled
};
```

Result: `true`

---

## String Concatenation

String concatenation is performed via `str.concat`, which requires importing `std.str`. String literals cannot be combined with `+`.

```toke
m=exprs;
i=str:std.str;
f=demo():$str{
  let first="hello";
  let second=" world";
  < str.concat(first;second)
};
```

Result: `"hello world"`

---

## Function Calls

A function call names a declared function and passes arguments in parentheses separated by semicolons.

```toke
m=exprs;
f=square(n:i64):i64{
  < n*n
};
f=demo():i64{
  < square(9)
};
```

Result: `81`

Arguments are evaluated left to right. The number and types of arguments must match the function's parameter list.

---

## Field Access — `.`

The dot operator accesses a named field on a struct, or a built-in property on a collection.

### Struct field

```toke
m=exprs;
t=$rect{w:i64;h:i64};
f=area(r:$rect):i64{
  < r.w*r.h
};
f=demo():i64{
  let r=$rect{w:4;h:5};
  < area(r)
};
```

Result: `20`

### Array length — `.len`

```toke
m=exprs;
f=demo():i64{
  let items=@(10;20;30);
  < items.len as i64
};
```

Result: `3` (`.len` returns `u64`; cast to `i64` for arithmetic with signed integers)

### Array element access — `.get(n)`

```toke
m=exprs;
f=demo():i64{
  let items=@(10;20;30);
  < items.get(1)
};
```

Result: `20` (zero-based index)

### Map value access — `.get(index)`

Map values are accessed by integer index in insertion order. Use structs for named-field access.

```toke
m=exprs;
f=demo():i64{
  let scores=@("alice":30;"bob":25);
  < scores.get(0)
};
```

Result: `30` (first entry)

---

## Cast Expressions — `as`

The `as` keyword performs an explicit type conversion. All casts are unconditionally accepted by the compiler. No implicit coercions exist — `as` is always required when types differ.

```toke
m=exprs;
f=demo():f64{
  let n:i64=7;
  let d:i64=2;
  < n as f64/d as f64
};
```

Result: `3.5`

Common cast patterns:

```toke
m=exprs;
f=demo():i64{
  let x:i64=42;
  let y:f64=x as f64;
  let z:u64=x as u64;
  let w:i32=x as i32;
  < x
};
```

---

## Error Match — `mt result {$ok:v ...; $err:e ...}`

The `mt` keyword introduces a match expression on an error union value, binding the success or error variant to a local name.

```toke
m=exprs;
i=file:std.file;
f=readsize(path:$str):i64{
  let result=file.read(path);
  < mt result {
    $ok:data data.len as i64;
    $err:e 0
  }
};
```

- `$ok:data` binds the success value to `data`
- `$err:e` binds the error value to `e`
- Both arms must produce the same type
- Arms are separated by `;`

The `mt` keyword also matches on sum types (tagged unions):

```toke
m=exprs;
t=$shape{$circle:f64;$square:f64};
f=area(s:$shape):f64{
  < mt s {
    $circle:r 3.14*r*r;
    $square:side side*side
  }
};
```

---

## Error Propagation — `expr!$errtype`

The `!` postfix operator unwraps an error union value. If the value is `$ok`, it returns the inner value. If it is `$err`, it immediately returns the error from the enclosing function.

```toke
m=exprs;
i=file:std.file;
f=linecount(path:$str):i64{
  let result=file.read(path);
  < mt result {$ok:data data.len as i64;$err:e 0}
};
```

The `!` propagation operator unwraps an error union: if the value is `$ok`, it returns the inner value; if `$err`, it propagates the error from the enclosing function. Using `!` in a function that does not return an error union produces error [E3020](/docs/reference/errors/#e3020).

---

## Operator Precedence

Operators bind from tightest to loosest:

| Precedence | Operator | Description |
|------------|----------|-------------|
| 1 (tightest) | `.` | Field access |
| 2 | `()` | Function call |
| 3 | `!` (postfix) | Error propagation |
| 4 | `-` (unary) | Negation |
| 5 | `as` | Type cast |
| 6 | `*` `/` | Multiply, divide |
| 7 | `+` `-` | Add, subtract |
| 8 | `<` `>` `=` | Comparison (use `!(a>b)` for `<=`, `!(a<b)` for `>=`) |
| 9 | `&&` | Logical AND |
| 10 (loosest) | `\|\|` | Logical OR |

Use parentheses to override precedence:

```toke
m=exprs;
f=demo(a:i64;b:i64;n:i64;d:i64):bool{
  let ok=(a>0)&&(b>0);
  let ratio=(n as f64)/(d as f64);
  < ok
};
```
