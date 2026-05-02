---
title: Hello World
slug: hello-world
section: learn
order: 11
---

## The program

Create a file called `hello.tk` with the following contents:

```
m=hello;
i=io:std.io;

f=main():i64{
  io.println("Hello, world!");
  <0;
};
```

That is a complete toke program. Let's break it down line by line.

## Line by line

### Module declaration

```
m=hello;
```

Every toke file starts with a module declaration. `m=` is the module keyword, followed by the module name. The semicolon terminates the declaration. This tells the compiler that this file belongs to the `hello` module.

### Import

```
i=io:std.io;
```

`i=` declares an import. The format is `i=alias:module.path;`. Here we import `std.io` (the standard I/O library) and bind it to the local alias `io`. After this line, we can call any function in `std.io` using the `io.` prefix.

### Function declaration

```text
f=main():i64{
```

`f=` declares a function. `main` is the function name. The empty parentheses `()` mean it takes no parameters. `:i64` is the return type -- a 64-bit signed integer. The opening `{` begins the function body.

Every toke program needs a `main` function that returns `i64`. The return value becomes the process exit code.

### Function body

```
  io.println("Hello, world!");
```

This calls the `println` function from the `io` module we imported. It prints the string to stdout followed by a newline. The semicolon terminates the statement.

### Return

```
  <0;
```

The `<` operator returns a value from the function. Here we return `0`, which means successful exit. toke also supports the longer `rt 0;` form for the same operation, but `<` is idiomatic for simple returns.

### Closing

```
};
```

The `}` closes the function body and the `;` terminates the function declaration. In toke, every top-level declaration ends with a semicolon.

## Compile and run

```bash
./build/tkc hello.tk -o hello
./hello
```

Output:

```
Hello, world!
```

The exit code is `0`:

```bash
echo $?
# 0
```

## What happened

When you ran `tkc hello.tk -o hello`, the compiler executed a five-stage pipeline:

1. **Lexing** -- The source text is converted into a flat stream of tokens. Whitespace separates tokens but has no other structural role -- indentation, line breaks, and spacing between tokens are all equivalent, and whitespace is discarded after tokenisation. The lexer identifies keywords like `m`, `f`, `i`, identifiers like `main` and `io`, literals like `"Hello, world!"` and `0`, and symbols like `=`, `:`, `{`, `<`, `;`.

2. **Parsing** -- The token stream is parsed into an abstract syntax tree (AST). toke's grammar is LL(1), meaning the parser never needs more than one token of lookahead to decide what production to apply. This makes parsing fast and deterministic.

3. **Type checking** -- The compiler verifies that every expression has a valid type. It checks that `main` returns `i64`, that `io.println` receives a `$str` argument, and that every code path returns a value.

4. **LLVM IR generation** -- The typed AST is lowered to LLVM intermediate representation. This is where toke hands off to the LLVM backend for optimisation and native code generation.

5. **Native binary** -- LLVM compiles the IR to machine code for your target platform and links it into a standalone executable. No runtime, no virtual machine, no garbage collector.

## A slightly more interesting example

Here is a program that computes the 10th Fibonacci number:

```
m=fib;

f=fib(n:i64):i64{
  if(n<2){<n;};
  <fib(n-1)+fib(n-2);
};

f=main():i64{
  <fib(10);
};
```

Compile and run:

```bash
./build/tkc fib.tk -o fib
./fib; echo $?
# 55
```

The `if(n<2){<n;};` line is a conditional: if `n` is less than 2, return `n`. The `if` keyword begins a conditional with the condition in parentheses and the body enclosed in braces.

## Next steps

Now that you have compiled your first programs, take the [Language Tour](/docs/learn/tour/) to see everything toke can do.
