---
title: Lesson 1 — Why toke?
slug: 01-why-toke
section: learn
order: 1
---

**Estimated time: ~20 minutes**

## The LLM code generation problem

Large language models generate code one token at a time. Every token costs compute, time, and money. When an LLM writes a Python function, most of those tokens are spent on syntactic boilerplate -- keywords like `def`, `return`, `import`, whitespace indentation, colons, and decorators. The actual logic is a fraction of the output.

This is not a problem with LLMs. It is a problem with the languages they are asked to generate.

## Token count: a concrete example

Consider a function that computes the nth Fibonacci number. Here it is in Python:

```python
def fib(n: int) -> int:
    if n < 2:
        return n
    return fib(n - 1) + fib(n - 2)
```

Under the `cl100k_base` tokenizer (used by GPT-4 and similar models), this function is **35 tokens**.

Here is the same function in toke:

```
m=fib;
f=fib(n:i64):i64{
  if(n<2){<n};
  <fib(n-1)+fib(n-2);
};
```

This is **36 tokens** under the same tokenizer -- nearly identical for a bare function. The cl100k tokenizer was trained on millions of Python programs, so `def`, `return`, and `print` are highly efficient single tokens. toke's syntax is unfamiliar to cl100k, so patterns like `f=` and `<` get split into separate tokens.

The real difference emerges in two scenarios:

**1. Complete programs.** Add `print(fib(10))` to Python (41 tokens total) vs toke's `f=main():i64{io.println(fib(10));<0};` (59 tokens total for the readable version). toke needs a module declaration and explicit main, but C (59 tokens) and Java (62 tokens) need similar boilerplate.

**2. Documentation.** Add a one-line docstring to every Python function and the count doubles. toke source is comment-free by design -- there is no comment syntax in the language. Documentation for human readers lives in companion files (`.tkc`), completely separate from source. The purpose-built model trains on clean code only, so toke's token cost is fixed. This is where toke's structural advantage compounds.

The syntactic differences are:

- **`f=` instead of `def`** -- single-character declaration prefix
- **`<` instead of `return`** -- one character, not six
- **`if(cond){body}` instead of `if cond:\n    body`** -- no indentation tokens, no colon
- **`;` terminates everything** -- no whitespace-as-syntax
- **No comments in source** -- documentation lives in companion files (`.tkc`), never in `.tk` source

## Why does this matter?

Three reasons:

### 1. Cost

LLM API pricing is per-token. toke has zero documentation overhead in source files -- there are no comments, period. The per-function token cost stays flat as coding standards increase. At scale -- thousands of generation calls per day -- this adds up fast.

### 2. Speed

Fewer output tokens means faster generation. A program that costs 77 tokens in toke (with descriptive names) versus 102 tokens in Java (with Javadoc) finishes proportionally faster.

### 3. Reliability

Shorter programs have fewer opportunities for the model to make mistakes. toke's LL(1) grammar and structured diagnostics mean the compiler can give the LLM precise, machine-readable feedback when something goes wrong -- enabling automated repair loops.

## More comparisons

Here is a function that sums an array of integers:

**Go (42 tokens, cl100k_base):**
```go
func sum(arr []int) int {
    total := 0
    for i := 0; i < len(arr); i++ {
        total += arr[i]
    }
    return total
}
```

**toke (49 tokens, cl100k_base):**
```
f=sum(arr:@i64):i64{
  let acc=mut.0;
  lp(let i=0;i<arr.len;i=i+1){
    acc=acc+arr.get(i);
  };
  <acc;
};
```

toke is slightly more expensive here on cl100k because the tokenizer splits toke-specific patterns like `lp(` and `arr.len` into multiple tokens. A purpose-built tokenizer trained on toke programs would merge these common patterns, narrowing the gap.

And an HTTP handler:

**Python Flask (67 tokens, cl100k_base):**
```python
from flask import Flask, request, jsonify

app = Flask(__name__)

@app.route("/user/<int:id>", methods=["GET"])
def get_user(id):
    user = db.get_user(id)
    if user is None:
        return jsonify({"error": "not found"}), 404
    return jsonify(user.to_dict())
```

**toke (84 tokens, cl100k_base):**
```
m=api.user;
i=http:std.http;
i=db:std.db;
i=json:std.json;

f=getuser(req:http.$req):http.$res!$apierr{
  let id=req.param("id") as u64;
  let user=db.one("SELECT * FROM users WHERE id=?";@(id))!$apierr;
  <http.$res.ok(json.enc(user));
};
```

toke is more expensive on cl100k for this example because the general-purpose tokenizer doesn't recognise toke's type sigils (`$req`, `$res`, `$apierr`) or import patterns. However, toke's program includes explicit imports, typed error handling, and a module declaration -- structural elements that Python defers to runtime. Add Flask's standard docstrings and type hints and the Python version grows significantly while toke's stays the same.

## Gate 1 results

toke's token efficiency claims are backed by empirical benchmarks from the Gate 1 evaluation:

- **12.5% token reduction** compared to equivalent Python programs, measured across 1,000 held-out benchmark tasks
- **63.7% Pass@1** -- the fraction of toke programs generated by a fine-tuned 7B model that compiled and passed tests on the first attempt, without a repair loop
- Repair loop convergence in under 2 rounds on average when Pass@1 fails

These numbers compare toke against Python using the same tokenizer (cl100k_base). The gap widens against verbose languages like Java, and widens further when documentation standards are applied.

## toke's place in the ecosystem

toke is not a replacement for Python, Go, Rust, or any other language. It occupies a specific niche:

**toke is a compilation target for LLM code generation.**

The workflow looks like this:

1. An LLM receives a task description
2. It generates toke source code (fewer tokens, faster, cheaper)
3. The `tkc` compiler compiles it to a native binary
4. If compilation fails, structured diagnostics are fed back to the LLM
5. The LLM repairs and resubmits (the "repair loop")

Humans can read and write toke -- it is designed to be learnable -- but its primary optimisation target is machine generation. This is what makes it different from every other language.

## The design constraints

toke achieves token efficiency through deliberate constraints:

- **55 characters** -- lowercase letters, digits, and 19 symbols. No uppercase, no underscores, no `#`
- **13 keywords** -- `f`, `t`, `i`, `m`, `if`, `el`, `lp`, `br`, `let`, `mut`, `as`, `rt`, `mt`
- **One way to do everything** -- no synonym constructs, no optional syntax
- **LL(1) grammar** -- deterministic parsing with one token of lookahead
- **Semicolons terminate everything** -- no whitespace-as-syntax
- **No comment syntax** -- source is comment-free by design; human documentation lives in companion files (`.tkc`)
- **Explicit types everywhere** -- no type inference in v0.1

These constraints make toke predictable, parseable, and compact.

## Exercise

Take this Python function:

```python
def maxofthree(a: int, b: int, c: int) -> int:
    if a >= b and a >= c:
        return a
    elif b >= c:
        return b
    return c
```

1. Count the approximate tokens (you can use OpenAI's tokenizer tool or estimate: roughly 1 token per word/symbol/number)
2. Rewrite it in toke using what you know so far (`f=`, `if`, `el`, `<`)
3. Count the toke tokens and compare

Do not worry about getting the syntax perfectly right yet -- the next lesson covers it in detail. The goal is to build intuition for where the savings come from.

## Key takeaways

- LLMs pay per token. Verbose languages waste tokens on syntax, not logic.
- toke source is comment-free by design. There are no comments in `.tk` files -- documentation for human readers lives in companion files (`.tkc`). Other languages double in token cost when documentation is added; toke's cost is fixed.
- On cl100k_base, toke matches C and beats Java for complete programs. Python is cheaper on cl100k because the tokenizer was trained on Python.
- A purpose-built tokenizer (Phase 2, projected) would merge toke-specific patterns into single tokens, significantly reducing token counts.
- Gate 1 benchmark: 12.5% token reduction and 63.7% Pass@1 across 1,000 held-out tasks.
- toke is a compilation target for LLM workflows, not a general-purpose replacement.
- Fewer tokens means lower cost, faster generation, and fewer errors.
- The language has 13 keywords, 55 characters, and one canonical form for every construct.

## Next

[Lesson 2: Modules and Functions](/docs/learn/02-modules-functions/) -- write your first toke programs.
