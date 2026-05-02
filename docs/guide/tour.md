---
title: Language Tour
slug: tour
section: learn
order: 15
---

This tour covers every major feature of the toke language. Each section includes a short explanation and a working code example.

## Modules and functions

Every toke file begins with a module declaration. Functions are declared with `f=`.

```
m=math;

f=square(x:i64):i64{
  <x*x;
};

f=main():i64{
  <square(7);
};
```

Functions explicitly declare parameter types and return types. There are no optional parameters, no overloading, and no default values. Every function has exactly one signature.

## Types

toke has a small set of built-in types:

| Type | Description |
|------|-------------|
| `i8`, `i16`, `i32`, `i64` | Signed integers |
| `u8`, `u16`, `u32`, `u64` | Unsigned integers |
| `f32`, `f64` | IEEE 754 floating point |
| `bool` | Boolean (`true` or `false`) |
| `$str` | UTF-8 string |
| `void` | No value (for functions with no meaningful return) |

You can also define struct types with `t=`:

```
t=$point{x:f64;y:f64};
```

And sum types (tagged unions) for error variants and enumerations:

```
t=$point{x:f64;y:f64};

t=$shape{
  $circle:f64;
  $rect:$point
};
```

## Let bindings and mutability

Use `let` for immutable bindings and `let x=mut.` for mutable ones:

```
f=example():i64{
  let x=10;
  let y=mut.20;
  y=y+x;
  <y;
};
```

Immutable bindings cannot be reassigned. Attempting to write `x=5;` after a `let` binding is a compile error.

## If / else

Conditionals use `if()` and `el{}`:

```
f=abs(n:i64):i64{
  if(n<0){
    <0-n;
  }el{
    <n;
  };
};
```

The condition goes inside parentheses after `if`. The else branch `el{...}` is optional. You can also write single-line conditionals:

```
f=max(a:i64;b:i64):i64{
  if(a>b){<a;}el{<b;};
};
```

For chained conditions, nest inside the else branch:

```
f=classify(n:i64):$str{
  if(n<0){
    <"negative";
  }el{
    if(n=0){
      <"zero";
    }el{
      <"positive";
    };
  };
};
```

## Loops

toke has exactly one loop construct: `lp`. It takes an initialiser, a condition, and a step:

```
f=sumto(n:i64):i64{
  let total=mut.0;
  lp(let i=0;i<n;i=i+1){
    total=total+i;
  };
  <total;
};
```

Use `br;` to break out of a loop early:

```
f=findfirstneg(arr:@i64):i64{
  let result=mut.0-1;
  lp(let i=0;i<arr.len;i=i+1){
    if(arr.get(i)<0){
      result=i;
      br;
    };
  };
  <result;
};
```

There is no `while`, `for-each`, or `do-while`. The `lp` construct covers all looping patterns.

## Match expressions

The `mt` keyword is used for pattern matching on sum types and error results:

```
t=$color{
  $red:bool;
  $green:bool;
  $blue:bool;
  $custom:$str
};

f=tohex(c:$color):$str{
  <mt c {
    $red:r "#FF0000";
    $green:g "#00FF00";
    $blue:b "#0000FF";
    $custom:s s
  };
};
```

Match is exhaustive -- the compiler rejects any match that does not cover all variants. When you add a new variant to a type, every match on that type must be updated or the program will not compile.

## Arrays and maps

Arrays use `@()` with semicolons as separators:

```
f=example():@i64{
  let nums=@(1;2;3;4;5);
  <nums;
};
```

Maps use key-value pairs with the same `@()` syntax as arrays, distinguished by `:` between key and value:

```
f=example():@($str:i64){
  let ages=@("alice":30;"bob":25;"carol":28);
  <ages;
};
```

Access array elements by index and map elements by key:

```
f=example():i64{
  let nums=@(10;20;30);
  let first=nums.get(0);
  <first;
};
```

Array types are written as `@T` (e.g. `@i64`) and map types as `@(K:V)` (e.g. `@($str:i64)`).

## Error handling

Functions that can fail declare an error type after `!`:

```
i=file:std.file;

t=$fileerr{
  $notfound:$str;
  $permdenied:$str
};

f=readfile(path:$str):$str!$fileerr{
  let content=file.read(path)!$fileerr;
  <content;
};
```

The `!` operator after a call propagates errors. If the call returns an error, the current function immediately returns that error. If the call succeeds, execution continues with the unwrapped value.

Use `mt` to match on the result and handle each case:

```
i=io:std.io;
i=file:std.file;

t=$fileerr{$notfound:$str;$permdenied:$str};

f=readfile(path:$str):$str!$fileerr{
  let content=file.read(path)!$fileerr;
  <content;
};

f=main():i64{
  let result=readfile("config.tk");
  mt result {
    $ok:content io.println(content);
    $err:e io.println("Failed to read file")
  };
  <0;
};
```

Error handling is explicit and checked by the compiler. You cannot ignore an error from a fallible function -- you must either propagate it with `!` or handle it with `mt`.

## Imports

Import other modules with `i=`:

```
m=app;
i=io:std.io;
i=json:std.json;
i=http:std.http;

f=main():i64{
  io.println(json.enc("hello"));
  <0;
};
```

The alias before the colon is how you refer to the module in code. The path after the colon is the fully qualified module name. All imports must appear before any type or function declarations.

## Putting it together

Here is a complete program that reads numbers from the command line and prints their sum:

```
m=sum;
i=io:std.io;

f=sum(arr:@i64):i64{
  let total=mut.0;
  lp(let i=0;i<arr.len;i=i+1){
    total=total+arr.get(i);
  };
  <total;
};

f=main():i64{
  let args=io.args();
  let result=sum(args);
  io.println(result as $str);
  <0;
};
```

## Next steps

Learn how to organise multi-file projects in [Project Structure](/docs/learn/project-structure/).
