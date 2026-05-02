---
title: Lesson 2 — Modules and Functions
slug: 02-modules-functions
section: learn
order: 2
---

**Estimated time: ~25 minutes**

## Every file starts with a module

Every toke source file begins with exactly one module declaration. No exceptions.

```
m=hello;
```

The `m=` keyword declares the module name. The name is a dot-separated path of lowercase identifiers. The semicolon terminates the declaration.

```text
m=hello;
m=api.user;
m=app.util.math;
```

These are single-segment, multi-segment, and deeper-nested module paths respectively.

The module name identifies the file within the project. Two files with the same module path are part of the same module and must not export conflicting names.

## Your first function

Functions are declared with `f=`:

```
m=hello;

f=main():i64{
  <0;
};
```

Let's break this down:

| Part | Meaning |
|------|---------|
| `f=` | Function declaration keyword |
| `main` | Function name |
| `()` | Parameter list (empty here) |
| `:i64` | Return type |
| `{<0;}` | Body -- returns 0 |
| `;` | Terminates the function declaration |

The `<` is the return operator. It returns the value of the expression that follows it. The `main` function returns `i64` by convention -- `0` for success, non-zero for failure.

Save this as `hello.tk` and compile it:

```bash
tkc hello.tk -o hello
./hello
echo $?  # prints 0
```

## Parameters and return types

Functions take typed parameters separated by semicolons:

```
m=math;

f=add(a:i64;b:i64):i64{
  <a+b;
};
```

Each parameter has the form `name:$type`. Multiple parameters are separated by `;` (not commas -- toke does not use commas anywhere).

The return type follows the closing parenthesis after a colon: `:i64`.

Here is a function with more parameters:

```
m=geometry;

f=rectarea(width:f64;height:f64):f64{
  <width*height;
};
```

## Functions that return nothing

Use the `void` return type for functions that perform side effects but return no value:

```
m=greet;
i=io:std.io;

f=sayhello(name:$str):void{
  io.println("Hello, \(name)!");
};
```

Note the string interpolation syntax: `\(expr)` inside a string literal inserts the value of `expr`.

## Multiple functions

A file can contain multiple function declarations. They are called in the usual way:

```
m=math;

f=square(x:i64):i64{
  <x*x;
};

f=sumofsquares(a:i64;b:i64):i64{
  <square(a)+square(b);
};

f=main():i64{
  let result=sumofsquares(3;4);
  <0;
};
```

Notice that function arguments in a call are also separated by `;`:

```
sumofsquares(3;4)
```

This is consistent with parameter declarations. Semicolons are the universal separator in toke.

## Bindings

Use `let` to create an immutable binding:

```
let x=42;
let name="Alice";
let area=width*height;
```

Once bound, the value cannot be changed. Attempting to reassign an immutable binding is a compile error.

Use `let` with `mut.` to create a mutable binding:

```
let counter=mut.0;
counter=counter+1;
counter=counter+1;
```

The `mut.` prefix marks the binding as mutable. After creation, you reassign with bare `name=value;`.

## Putting it together

Here is a complete program that computes the absolute value of an integer:

```
m=abs;
i=io:std.io;

f=abs(n:i64):i64{
  if(n<0){
    <0-n;
  };
  <n;
};

f=main():i64{
  let val=abs(0-42);
  io.println(val as $str);
  <0;
};
```

Key observations:

- The module declaration comes first
- Imports come next (before any functions)
- Functions are declared with `f=`
- `<` returns a value
- `;` terminates every statement, every block, and every declaration
- `if` checks a condition; we will cover it fully in the next lesson

## The file structure rule

toke enforces a strict ordering within every source file:

1. **Module declaration** (exactly one, required)
2. **Import declarations** (zero or more)
3. **Type declarations** (zero or more)
4. **Constant declarations** (zero or more)
5. **Function declarations** (zero or more)

Declarations must appear in this order. You cannot put an import after a function. The compiler enforces this and gives a clear error if you violate it.

## Exercises

### Exercise 1: Temperature converter

Write a function `f=ctof(c:f64):f64` that converts Celsius to Fahrenheit using the formula `f = c * 9/5 + 32`. Write a `main` function that converts 100 degrees and prints the result.

### Exercise 2: Distance formula

Write a function `f=distance(x1:f64;y1:f64;x2:f64;y2:f64):f64` that computes the Euclidean distance between two points. You can use `(dx*dx+dy*dy)` for now -- we will cover square root from the stdlib later.

### Exercise 3: Greeting

Write a module `greet` with:
- A function `f=greet(name:$str;times:i64):void` that prints "Hello, {name}!" the specified number of times (use a mutable counter and `if` to check the count -- we will cover loops properly next lesson)
- A `main` function that calls `greet("World";3)`

## Key takeaways

- Every file starts with `m=name;`
- Functions use `f=name(params):$rettype{body};`
- Parameters are separated by `;`, not commas
- `<expr;` returns a value from a function
- `let x=expr;` creates an immutable binding; `let x=mut.expr;` creates a mutable one
- File structure is strictly ordered: module, imports, types, constants, functions
- `;` terminates everything

## Next

[Lesson 3: Control Flow](/docs/learn/03-control-flow/) -- conditionals, loops, and match expressions.
