---
title: Language Reference
slug: grammar
section: reference
order: 6
---

# toke Language Reference

A complete guide to toke's syntax. Every construct is shown with working code examples you can copy and compile.

---

## File Structure

Every toke file follows a strict order. The compiler enforces this — you cannot put imports after functions or types before imports.

| Order | Keyword | Purpose | Required? |
|-------|---------|---------|-----------|
| 1 | `m=` | Module declaration | Yes (exactly one) |
| 2 | `i=` | Imports | Optional |
| 3 | `t=` | Type declarations | Optional |
| 4 | `f=` | Function declarations | Optional |

```toke
m=myapp;                            (* 1. module — always first *)
i=http:std.http;                    (* 2. imports *)
i=j:std.json;

t=$config{port:i64;host:str};      (* 3. types *)

f=main():i64{                      (* 4. functions *)
  http.serve(8080);
  <0
};
```

---

## Module Declaration

Every file starts with `m=name;`. The name is a dot-separated path that identifies the module.

```toke
m=api.users;
```

---

## Imports

Import a module and give it a local alias. All access to the module uses the alias.

```toke
i=http:std.http;          (* import std.http, access via http.* *)
i=j:std.json;             (* import std.json, access via j.* *)
i=db:std.db;              (* import std.db, access via db.* *)
```

After importing, use the alias to call functions:

```toke
let body=j.parse(input);
let res=http.ok(body);
```

---

## Functions

Declared with `f=name(params):returntype{body};`. Parameters are separated by semicolons. The return operator is `<`.

```toke
f=add(a:i64;b:i64):i64{
  <a+b
};
```

### Multiple parameters

```toke
f=clamp(val:i64;lo:i64;hi:i64):i64{
  if(val<lo){<lo};
  if(val>hi){<hi};
  <val
};
```

### No parameters

```toke
f=greet():i64{
  io.println("hello");
  <0
};
```

### Fallible functions (error handling)

A function that can fail declares its error type with `!`:

```toke
f=fetch(id:i64):$user!$apierr{
  let row=db.one("select * from users where id=?";@(id))!$apierr;
  <$user{id:row.id;name:row.name}
};
```

The `!` operator propagates errors — if the call fails, the function returns the error immediately.

---

## Types

### Scalar types (no sigil)

| Type | Description | Example |
|------|-------------|---------|
| `i64` | 64-bit signed integer | `let x=42;` |
| `i32`, `i16`, `i8` | Smaller integers | `let b=0 as i8;` |
| `u64`, `u32`, `u16`, `u8` | Unsigned integers | `let c=255 as u8;` |
| `f64`, `f32` | Floating point | `let pi=3.14159;` |
| `bool` | Boolean | `let ok=true;` |
| `str` | String | `let s="hello";` |

### Struct types (`$` prefix)

Define a struct with `t=$name{fields};`. Access fields with `.`:

```toke
t=$point{x:i64;y:i64};

f=dist(p:$point):i64{
  <p.x*p.x+p.y*p.y
};

f=origin():$point{
  <$point{x:0;y:0}
};
```

### Sum types (error variants)

When all fields start with `$`, the type is a sum type (tagged union):

```toke
t=$result{$ok:i64;$err:str};

f=divide(a:i64;b:i64):$result{
  if(b=0){<$result{$err:"division by zero"}};
  <$result{$ok:a/b}
};
```

### Array type (`@`)

```toke
let nums=@(1;2;3);          (* array literal *)
let first=nums.get(0);       (* read element — never nums[0] *)
let size=nums.len;            (* length — property, no parens *)
```

| Operation | Syntax | Notes |
|-----------|--------|-------|
| Create | `@(1;2;3)` | Semicolons separate elements |
| Read | `arr.get(i)` | Never `arr[i]` |
| Write | `arr.set(i;val)` | |
| Length | `arr.len` | Property, not function call |
| Type annotation | `@i64` | Array of i64 |

### Map type (`@(key:val)`)

```toke
let ages=@("alice":30;"bob":25);
let age=ages.get("alice");
```

| Operation | Syntax |
|-----------|--------|
| Create | `@("key":val;"key2":val2)` |
| Type annotation | `@(str:i64)` |

---

## Bindings

### Immutable (default)

```toke
let x=42;                     (* cannot be reassigned *)
let name="toke";
```

### Mutable

```toke
let count=mut.0;               (* mut. prefix on initial value *)
count=count+1;                 (* reassignment allowed *)
```

### With type annotation

```toke
let pi:f64=3.14159;
let flags:@bool=@(true;false;true);
```

---

## Control Flow

### Conditional (`if` / `el`)

```toke
if(x>0){
  io.println("positive")
}el{
  io.println("non-positive")
};
```

Nested conditionals (no `elif` — use nested `el{if`):

```toke
if(x>0){
  io.println("positive")
}el{
  if(x=0){
    io.println("zero")
  }el{
    io.println("negative")
  }
};
```

### Loop (`lp`)

toke has exactly one loop construct: `lp(init;condition;step){body}`.

```toke
lp(let i=0;i<10;i=i+1){
  io.println(i)
};
```

Break out of a loop with `br`:

```toke
lp(let i=0;true;i=i+1){
  if(i>100){br};
  io.println(i)
};
```

### Match (`mt`)

Match on sum types to handle variants:

```toke
mt getuser(id) {
  $ok:user  io.println(user.name);
  $err:e    io.println(e)
};
```

Match is an expression — it produces a value:

```toke
let name=mt lookup(id) {
  $ok:u   u.name;
  $err:e  "unknown"
};
```

---

## Return

Two forms, both produce the same result:

```toke
f=add(a:i64;b:i64):i64{
  <a+b                          (* short form — preferred *)
};

f=add2(a:i64;b:i64):i64{
  rt a+b;                       (* long form — clearer in nested code *)
};
```

---

## Operators

From highest to lowest precedence:

| Precedence | Operator | Description | Example |
|------------|----------|-------------|---------|
| 1 (highest) | `.` | Field access | `p.x` |
| 2 | `()` | Function call | `add(1;2)` |
| 3 | `!` | Error propagation | `parse(s)!$err` |
| 4 | `-` (unary) | Negation | `-x` |
| 5 | `as` | Type cast | `x as f64` |
| 6 | `*` `/` `%` | Multiply, divide, modulo | `a*b` |
| 7 | `+` `-` | Add, subtract | `a+b` |
| 8 | `<` `>` `=` | Compare, equality | `a>b`, `a=b` |
| 9 | `&&` | Logical AND | `a>0 && b>0` |
| 10 (lowest) | `\|\|` | Logical OR | `a=0 \|\| b=0` |

Note: `=` is both assignment (at statement level) and equality comparison (in expressions). Context disambiguates — no `==` operator.

---

## Separators

toke uses **semicolons everywhere**. Never commas.

```toke
f=add(a:i64;b:i64):i64{       (* parameters: semicolons *)
  <a+b
};

let r=add(1;2);                 (* arguments: semicolons *)
let arr=@(10;20;30);            (* array elements: semicolons *)
let m=@("a":1;"b":2);           (* map entries: semicolons *)
```

---

## Keywords

toke has 13 keywords. 4 are context-sensitive (only special when followed by `=` at the top level), 9 are reserved everywhere.

### Context keywords (declaration prefixes)

| Keyword | Purpose | Example |
|---------|---------|---------|
| `m=` | Module declaration | `m=myapp;` |
| `f=` | Function declaration | `f=add(a:i64):i64{...};` |
| `t=` | Type declaration | `t=$point{x:i64;y:i64};` |
| `i=` | Import declaration | `i=http:std.http;` |

Inside function bodies, `m`, `f`, `t`, `i` are valid variable names.

### Reserved keywords

| Keyword | Purpose | Example |
|---------|---------|---------|
| `let` | Immutable binding | `let x=42;` |
| `mut` | Mutable qualifier | `let x=mut.0;` |
| `if` | Conditional | `if(x>0){...}` |
| `el` | Else branch | `}el{...}` |
| `lp` | Loop | `lp(init;cond;step){...}` |
| `br` | Break | `br;` |
| `rt` | Return (long form) | `rt expr;` |
| `as` | Type cast | `x as f64` |
| `mt` | Match expression | `mt expr {...}` |

---

## Character Set

toke source uses exactly 55 ASCII characters:

| Category | Characters | Count |
|----------|-----------|-------|
| Letters | `a` through `z` | 26 |
| Digits | `0` through `9` | 10 |
| Symbols | `( ) { } = : . ; + - * / < > ! \| & $ @ % "` | 19 |

Plus space and newline as whitespace. No uppercase letters, no underscores, no square brackets, no commas.

---

## Syntax Profiles

| Feature | Default (55-char) | Legacy (`--legacy`, 80-char) |
|---------|-------------------|------------------------------|
| Declaration keywords | `m=` `f=` `t=` `i=` | `M=` `F=` `T=` `I=` |
| Type names | `$user` | `User` |
| Array literal | `@(1;2;3)` | `[1;2;3]` |
| Array type | `@i64` | `[i64]` |
| Array indexing | `arr.get(0)` | `arr[0]` |

The default syntax is the only syntax used for training and is the target for all tooling.

---

## Formal Grammar

The complete formal grammar in EBNF notation is available at [grammar.ebnf](https://github.com/karwalski/toke/blob/main/docs/spec/grammar.ebnf) for compiler implementers. This page presents the same information in human-readable form.
