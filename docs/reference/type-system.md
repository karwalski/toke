---
title: Type System
slug: type-system
section: reference
order: 13
---

toke uses a static, nominally-typed type system. Every value has a single concrete type known at compile time. There are no implicit coercions between numeric types, no null values, and no runtime type erasure.

---

## 1. Primitive types

| Type   | Size    | Range / Values                                                      | Zero value | Literal form |
|--------|---------|---------------------------------------------------------------------|------------|--------------|
| `i8`   | 8 bits  | −128 to 127                                                        | `0`        | `42`, `-1`   |
| `i16`  | 16 bits | −32,768 to 32,767                                                  | `0`        | `42`, `-1`   |
| `i32`  | 32 bits | −2,147,483,648 to 2,147,483,647                                    | `0`        | `42`, `-1`   |
| `i64`  | 64 bits | −9,223,372,036,854,775,808 to 9,223,372,036,854,775,807           | `0`        | `42`, `-1`   |
| `u8`   | 8 bits  | 0 to 255                                                            | `0`        | `0 as u8`    |
| `u16`  | 16 bits | 0 to 65,535                                                         | `0`        | `0 as u16`   |
| `u32`  | 32 bits | 0 to 4,294,967,295                                                  | `0`        | `0 as u32`   |
| `u64`  | 64 bits | 0 to 18,446,744,073,709,551,615                                    | `0`        | `0 as u64`   |
| `f32`  | 32 bits | IEEE 754 single-precision                                           | `0.0`      | `3.14 as f32`|
| `f64`  | 64 bits | IEEE 754 double-precision                                           | `0.0`      | `3.14`       |
| `bool` | 1 byte  | `true` or `false`                                                   | `false`    | `true`       |
| `$str` | pointer | Immutable UTF-8 string, arbitrary length                           | `""`       | `"hello"`    |
| `void` | 0 bytes | Unit type — functions that produce no value                        | n/a        | (none)       |

Integer literals are `i64` by default. Floating-point literals are `f64` by default. Use `as` to produce a narrower or unsigned type.

**Example:**

```toke
m=primitives;
f=demo():void{
  let age:i64 = 30;
  let ratio:f64 = 1.618;
  let flag:bool = true;
  let name:$str = "toke";
  let byte:u8 = 255 as u8;
};
```

---

## 2. The `$` sigil

All named types — user-defined structs, sum types, and the built-in `$str` — are written with a `$` prefix: `$point`, `$str`, `$err`, `$shape`.

**Why `$`?**

- It makes type positions unambiguous in the grammar without requiring a keyword.
- At 55-char density, it acts as a single-character signal that the following token is a type name, not a variable or function name.
- There are no bare named types without `$`. `point` is always a variable; `$point` is always a type.

```toke
m=test;
t=$point{x:i64;y:i64};

f=makepoint(x:i64;y:i64):$point{
  < $point{x:x;y:y}
};
```

The `$` appears in the declaration (`t=$point{...}`), in type annotations (`: $point`), and in struct literals (`$point{x:0;y:0}`).

---

## 3. Struct types

A struct is a named product type. It groups named fields into a single value.

**Declaration:**

```toke
m=test;
t=$point{x:i64;y:i64};
```

**Instantiation:**

```toke
m=test;
t=$point{x:i64;y:i64};
f=example():void{
  let p = $point{x:3;y:4};
};
```

**Field access:**

```toke
m=test;
t=$point{x:i64;y:i64};
f=example():void{
  let p = $point{x:3;y:4};
  let xval = p.x;
};
```

**Worked example:**

```toke
m=geometry;
i=math:std.math;

t=$point{x:i64;y:i64};

f=distance(a:$point;b:$point):f64{
  let dx = (a.x - b.x) as f64;
  let dy = (a.y - b.y) as f64;
  < math.sqrt(dx*dx + dy*dy)
};

f=demo():void{
  let origin = $point{x:0;y:0};
  let target = $point{x:3;y:4};
  let d = distance(origin;target);
};
```

Two struct types are equal only when they share the same name (nominal typing). Two structs with identical fields but different names are distinct types.

---

## 4. Sum types (tagged unions)

A sum type holds exactly one of several named variants at runtime. Each variant (tag) carries a payload of a specified type.

**Declaration:**

```toke
m=test;
t=$pair{w:f64;h:f64};
t=$shape{$circle:f64;$rect:$pair};
```

Tags use the `$` sigil prefix; struct fields use lowercase without `$`. This is how the compiler distinguishes a sum type from a struct.

**Matching:**

```toke
m=test;
t=$pair{w:f64;h:f64};
t=$shape{$circle:f64;$rect:$pair};

f=area(s:$shape):f64{
  < mt s {
    $circle:r  r*r*3.14159;
    $rect:p    p.w*p.h
  }
};
```

Each arm of `mt expr {...}` binds the tag's payload to a local name (here `r` and `p`). Match is exhaustive — omitting a tag is a compile error.

**Worked example:**

```toke
m=shapes;
i=str:std.str;

t=$pair{w:f64;h:f64};
t=$shape{$circle:f64;$rect:$pair};

f=describe(s:$shape):$str{
  < mt s {
    $circle:r  str.concat("circle r=";str.fromfloat(r));
    $rect:p    str.concat("rect ";str.concat(str.fromfloat(p.w);"x..."))
  }
};

f=demo():void{
  let c = $shape{$circle:5.0};
  let r = $shape{$rect:$pair{w:3.0;h:4.0}};
  let dc = describe(c);
  let dr = describe(r);
};
```

---

## 5. Error unions — `T!$errtype`

An error union represents either a success value of type `T` or an error of type `$errtype`. The common error type from the standard library is `$err`.

**Function signature:**

```toke
m=test;
t=$err{msg:$str};
f=readfile(path:$str):$str!$err{
  <"";
};
```

**Error propagation with `!`:**

```toke
m=test;
t=$err{msg:$str};
f=readfile(path:$str):$str!$err{
  <"";
};
f=loadconfig(path:$str):$str!$err{
  let raw = readfile(path)!$err;
  < raw
};
```

The `!$errtype` postfix on a call unwraps the success value or propagates the error to the caller. The enclosing function must also return an error union type.

**Error matching:**

```toke
m=test;
t=$err{msg:$str};
f=readfile(path:$str):$str!$err{
  <"";
};
f=saferead(path:$str):$str{
  let result = readfile(path);
  < mt result {
    $ok:content  content;
    $err:e       "default"
  }
};
```

**Worked example:**

```toke
m=fileutil;
i=io:std.io;
i=str:std.str;

t=$err{msg:$str};

f=firstline(path:$str):$str!$err{
  let content = io.readfile(path)!$err;
  let lines = str.splitlines(content);
  if(lines.len=0 as u64){
    < $err{msg:"empty file"}
  }el{
    < lines.get(0)
  }
};
```

---

## 6. Array types — `@T`

An array is an ordered, homogeneous sequence of elements of type `T`. The type is written `@T` (e.g. `@i64`, `@$str`).

**Literal syntax:**

```toke
m=test;
f=example():void{
  let nums:@i64 = @(1;2;3);
  let empty:@i64 = @();
};
```

**Access:**

```toke
m=test;
f=example():void{
  let nums:@i64 = @(1;2;3);
  let first = nums.get(0);
  let count = nums.len;
};
```

- `.get(n)` — element at index `n` (type `u64`)
- `.len` — number of elements (type `u64`)
- There is no `arr[n]` subscript syntax in toke.

**Worked example:**

```toke
m=arrsum;

f=sum(vals:@i64):i64{
  let acc=mut.0;
  lp(let i=0 as u64;i<vals.len;i=i+1 as u64){
    acc=acc+vals.get(i);
  };
  < acc
};

f=demo():void{
  let data = @(10;20;30;40);
  let total = sum(data);
};
```

---

## 7. Map types — `@($str:T)`

A map is a key-value collection where keys are always `$str` and all values have the same type `T`. The type is written `@($str:T)`.

**Literal syntax:**

```toke
m=test;
f=example():void{
  let ages:@($str:i64) = @("alice":30;"bob":25);
};
```

**Access:**

```toke
m=test;
f=example():void{
  let ages:@($str:i64) = @("alice":30;"bob":25);
  let aliceage = ages.get("alice");
  let count = ages.len;
};
```

- `.get(key)` — value for key (type `$str`)
- `.len` — number of entries (type `u64`)
- There is no `map[key]` subscript syntax in toke.

**Worked example:**

```toke
m=scores;
i=str:std.str;

f=topscore(board:@($str:i64)):i64{
  let best=mut.0;
  lp(let i=0 as u64;i<board.len;i=i+1 as u64){
    let v = board.get(str.fromint(i as i64));
    if(v>best){ best=v; };
  };
  < best
};
```

---

## 8. Type inference

toke infers the type of a binding from its initializer expression. No annotation is required when the type is unambiguous.

| Expression form | Inferred type | Notes |
|-----------------|---------------|-------|
| Integer literal `42` | `i64` | Default integer type |
| Float literal `3.14` | `f64` | Default float type |
| String literal `"hi"` | `$str` | |
| Boolean literal `true` | `bool` | |
| Array literal `@(1;2;3)` | `@i64` | Element type from first element |
| Empty array `@()` | `@unknown` | Annotation required |
| Map literal `@("k":v)` | `@($str:V)` | Key always `$str`, value from first entry |
| Struct literal `$name{...}` | `$name` | From type declaration |
| Identifier | declared type | From binding, parameter, or function |
| Call `f(args)` | return type | From function declaration |
| Field access `expr.field` | field type | Struct field or `.len` |
| Cast `expr as T` | `T` | Explicit target type |

**Worked example:**

```toke
m=infer;

f=demo():void{
  let x = 42;
  let y = 3.14;
  let s = "hello";
  let ok = true;
  let ns = @(1;2;3);
  let m = @("a":1;"b":2);
};
```

When inference is ambiguous (e.g. empty array), add a type annotation:

```toke
m=test;
f=example():void{
  let tags:@$str = @();
};
```

---

## 9. Casting — `expr as targettype`

Use `as` to convert between numeric types. There are no implicit coercions.

**Widening:**

```toke
m=test;
f=example():void{
  let n:i64 = 42;
  let f:f64 = n as f64;
};
```

**Narrowing:**

```toke
m=test;
f=example():void{
  let big:i64 = 300;
  let byte:u8 = big as u8;
};
```

**Common casts:**

```toke
m=test;
f=example(somei64:i64;measured:f64):void{
  let count:u64 = somei64 as u64;
  let index:u64 = 0 as u64;
  let ratio:f32 = measured as f32;
};
```

Casting between non-numeric types (e.g. `$str as i64`) is a compile error. Use standard library parse functions instead.

**Worked example:**

```toke
m=casts;

f=average(vals:@i64):f64{
  let total=mut.0;
  lp(let i=0 as u64;i<vals.len;i=i+1 as u64){
    total=total+vals.get(i);
  };
  < (total as f64) / (vals.len as f64)
};
```

---

## See also

- [Types reference](/docs/reference/types/) — concise type reference with compatibility rules
- [Expressions](/docs/reference/expressions/) — operators, casts, field access, and `.get()`
- [Error handling](/docs/reference/error-handling/) — error union propagation and matching
- [Migration guide](/docs/reference/migration/) — moving from legacy `[]` and `fn` syntax
