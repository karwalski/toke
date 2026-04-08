# toke: Research Review Request

**Author:** Matt Watt (karwalski)
**Date:** April 2026
**Status:** Post-Gate 1 — PASS — Requesting Feedback
**Repository:** [github.com/karwalski/toke](https://github.com/karwalski/toke)

---

## Purpose of This Document

This document invites research review and feedback on **toke**, a compiled programming language designed as a code generation target for large language models. The project has passed its first evaluation gate (Gate 1) and we are seeking input from researchers and practitioners in:

- Programming language design
- LLM code generation and evaluation
- Compiler construction
- Tokenizer design and BPE methodology
- AI training data curation

We are requesting review of the language design, syntax decisions, evaluation methodology, and Gate 1 results as the project enters Phase 2 (Language Extensions). Feedback at this stage will inform Phase 2 design decisions and model scaling strategy.

---

## 1. The Problem

LLMs generate code token by token. Every token costs compute, time, and money. Current programming languages (Python, C, Java, TypeScript) were designed for human authors and carry structural overhead that inflates token counts:

- Verbose keywords (`function`, `return`, `import` — multi-token each)
- Optional and ambiguous syntax (semicolons, parentheses, whitespace significance)
- Multiple valid forms for equivalent constructs
- Ambiguous grammars requiring backtracking parsers
- Human-readable error messages that models must parse as natural language

## 2. The Thesis

> A sufficiently constrained, unambiguous, token-efficient language will reduce end-to-end LLM code generation cost — measured as (tokens x iterations x error rate) — by a material margin sufficient to justify building and training a purpose-native model.

This is treated as an empirical hypothesis with explicit go/no-go gates.

## 3. Language Overview

### 3.1 Character Set

toke uses **56 ASCII characters**: 26 lowercase letters, 10 digits, and 20 symbols. No uppercase letters appear in source code.

```
a-z    0-9
( ) { } ; : . , " * / + - < > = ! | $ @
```

Two characters (`^`, `~`) are reserved for v0.2 features.

### 3.2 Keywords (12 total)

| Keyword | Meaning | Replaces |
|---------|---------|----------|
| `m` | Module declaration | `module`, `package` |
| `f` | Function declaration | `func`, `def`, `fn` |
| `t` | Type declaration | `type`, `struct`, `enum` |
| `i` | Import | `import`, `use`, `require` |
| `c` | Constant | `const`, `final` |
| `if` | Conditional | `if` |
| `el` | Else branch | `else`, `elif` |
| `lp` | Loop | `for`, `while`, `loop` |
| `br` | Break | `break` |
| `let` | Binding | `let`, `var` |
| `mut` | Mutable binding | `var`, `mut` |
| `as` | Type cast | `as` |

The return statement uses `<` (one character vs six for `return`).

### 3.3 Grammar

LL(1) — deterministic parsing with exactly one token of lookahead. No backtracking, no ambiguity. Every syntactic position has exactly one valid interpretation.

Full EBNF: [spec/grammar.ebnf](https://github.com/karwalski/toke/blob/main/spec/spec/grammar.ebnf)

### 3.4 Type System

Statically typed. Explicit type annotations on all function signatures. Six primitive types:

| Type | Width | Description |
|------|-------|-------------|
| `i64` | 64-bit | Signed integer |
| `u64` | 64-bit | Unsigned integer |
| `f64` | 64-bit | IEEE 754 double |
| `bool` | 1 byte | Boolean |
| `$str` | pointer | Immutable UTF-8 string |
| `void` | 0 bytes | Unit type |

Composite types use sigil prefixes:
- `$typename` — user-defined struct or sum type
- `@T` — array of T
- `$(K:V)` — map from K to V
- `T!$err` — error union (result or error)

### 3.5 Example Program

```
m=api;
i=http:std.http;
i=json:std.json;

t=$apierr{
  $notfound:u64;
  $badrequest:$str
};

f=handle(req:http.$req):http.$res!$apierr{
  let id=json.dec(req.body)!$apierr;
  let users=$($str:i64)("alice":1;"bob":2);
  <http.$res.ok(json.enc(users));
};
```

### 3.6 Structured Diagnostics

The compiler emits JSON diagnostics with stable error codes, machine-parseable source locations, and mechanically derivable fix suggestions. 70+ error codes across 7 compilation stages. No prose error messages — every diagnostic is structured for automated repair loops.

```json
{
  "code": "E2003",
  "severity": "error",
  "stage": "parse",
  "message": "expected ';' after statement",
  "span": {"file": "main.tk", "line": 4, "col": 12, "len": 1},
  "fix": "insert ';' at position 4:12"
}
```

---

## 4. What Has Been Built

| Component | Status | Repository |
|-----------|--------|------------|
| Language specification (v0.1) | Complete | [toke/spec/](https://github.com/karwalski/toke/tree/main/spec) |
| Reference compiler (C99, LLVM backend) | Complete | [toke/src/](https://github.com/karwalski/toke/tree/main/src) |
| Standard library (14 modules) | Complete | [toke/stdlib/](https://github.com/karwalski/toke/tree/main/stdlib) |
| Training corpus (46,754 programs) | Complete | [toke-model/corpus/](https://github.com/karwalski/toke-model/tree/main/corpus) |
| BPE tokenizer (8K/32K vocab) | Complete | [toke-model/tokenizer/](https://github.com/karwalski/toke-model/tree/main/tokenizer) |
| Fine-tuned model (Qwen 2.5 Coder 7B + LoRA) | Complete | [toke-model](https://github.com/karwalski/toke-model) |
| Benchmark harness (1,000 held-out tasks) | Complete | [toke-eval/benchmark/](https://github.com/karwalski/toke-eval/tree/main/benchmark) |
| Gate 1 evaluation | **PASS** | [gate1-decision.md](https://github.com/karwalski/toke/blob/main/spec/docs/gate1-decision.md) |

### 4.1 Compiler

Single-pass C99 compiler: lexer, LL(1) parser, name resolution, type inference, LLVM IR codegen (x86-64, ARM64). `--check` mode for corpus validation. 90 conformance tests, 9 e2e tests. Targets: x86-64 Linux, ARM64 Linux, ARM64 macOS.

### 4.2 Training Corpus

46,754 validated, deduplicated, compiler-checked programs across four stages:

| Stage | Count | Description |
|-------|-------|-------------|
| A — Single-function | 26,978 | Core algorithms (array, string, math, conditional, error) |
| B — Composition | 9,776 | Multi-function programs combining Stage A functions |
| C — Edge cases | 5,000 | Boundary conditions, error propagation, recovery patterns |
| D — Applications | 5,000 | Multi-function application-level programs |

**Generation methodology:** Multi-model pipeline (Claude Haiku 4.5, GPT-4.1-mini, Grok-3-mini). Each program independently validated by the compiler and cross-checked via differential testing against reference implementations in Python, C, and Java (majority agreement required).

**Pass@1 rate during generation:** 62% (Stage A), with auto-fixer rescuing ~15% of failures at zero API cost.

### 4.3 Standard Library

14 modules with C runtime backing:

`std.str`, `std.json`, `std.toon`, `std.yaml`, `std.i18n`, `std.http`, `std.db`, `std.file`, `std.env`, `std.process`, `std.crypto`, `std.time`, `std.log`, `std.test`

toke uses a **TOON-first serialization strategy**: TOON (Token-Oriented Object Notation) as the default format for tabular data (30-60% fewer tokens than JSON), with YAML and JSON as secondary formats. String externalisation for internationalisation is handled via `std.i18n` with locale-aware bundle loading across all three formats. See [ADR-0003](https://github.com/karwalski/toke/blob/main/spec/docs/architecture/ADR-0003.md).

---

## 5. Preliminary Results

### 5.1 Tokenizer Evaluation

BPE tokenizer trained on the legacy profile corpus (46,730 programs). Evaluated on 4,675 held-out programs.

| Metric | 8K Vocabulary | 32K Vocabulary |
|--------|--------------|----------------|
| Token reduction vs cl100k_base | **12.5%** | **13.1%** |
| Mean tokens (toke BPE) | 172.9 | 171.8 |
| Mean tokens (cl100k baseline) | 197.6 | 197.6 |
| Compression ratio | 0.875 | 0.869 |
| Vocabulary utilisation | 70.2% | 23.5% |
| Fertility | 0.377 | 0.374 |

> **Note:** These results are from the legacy profile (80-character) corpus. The default syntax (56-character) corpus transformation is complete (46,754 entries transformed). The reduced character set is expected to improve these numbers further.

### 5.2 Token Efficiency vs Other Languages

Measured on equivalent programs using cl100k_base tokenizer:

| Language | Mean tokens (function only) | Mean tokens (complete program) |
|----------|-----------------------------|-------------------------------|
| toke (cl100k) | ~38 | ~52 |
| Python | ~85 | ~156 |
| C | ~60 | ~168 |
| Java | ~43 | ~127 |

With the purpose-built tokenizer, toke's complete program drops to ~19 tokens for equivalent logic (projected, based on 8K vocabulary evaluation).

### 5.3 Gate 1 Results — PASS (2026-04-03)

| Criterion | Threshold | Result | Verdict |
|-----------|-----------|--------|---------|
| Token reduction vs cl100k_base | > 10% | 12.5% (8K vocab) / 13.1% (32K vocab) | **PASS** |
| Pass@1 on held-out tasks | >= 60% | 63.7% (588/923 tasks) | **PASS** |

**Benchmark details:** 1,000 held-out tasks with 120 test inputs each. 923/1,000 compiled successfully (92.3%). Of those, 588 passed all test cases on first generation (63.7% Pass@1). Model: Qwen 2.5 Coder 7B with QLoRA adapter, inference on Mac Studio M4 Max (41.7 minutes for 1,000 tasks).

**Cross-language token comparison** (cl100k_base, complete programs):

| Language | Mean tokens | vs toke |
|----------|------------|---------|
| **toke** | **52** | baseline |
| Python | 156 | 3.0x more |
| C | 168 | 3.2x more |
| Java | 127 | 2.4x more |

Full Gate 1 decision document: [gate1-decision.md](https://github.com/karwalski/toke/blob/main/spec/docs/gate1-decision.md)

---

## 6. Design Decisions Requesting Feedback

We specifically invite critique and alternative perspectives on the following decisions:

### 6.1 Character Set Reduction (Legacy 80 → Default 56 characters)

Removing uppercase letters eliminates case-based type/value distinction (common in Go, Haskell, Rust). Instead, toke uses `$` sigils for type names (`$user`, `$str`). This reduces the character set but increases average identifier length by 1 character.

**Question:** Is the sigil approach the right trade-off? Are there better ways to distinguish types from values in a case-insensitive language?

### 6.2 Single Loop Construct

toke has one loop: `lp(init;cond;step){body}`. No `for-each`, `while`, `do-while`, or iterator protocol. All looping patterns are expressed through `lp`.

**Question:** Does the absence of iteration abstractions measurably increase token counts for collection-heavy programs? Should a `for-each` equivalent be added?

### 6.3 No Comments in Source

toke source has no comment syntax. Metadata lives outside the source file. The rationale: comments are invisible to the compiler and consume tokens that carry no semantic value.

**Question:** Does the absence of comments affect LLM generation quality? Do models use self-generated comments as a reasoning scaffold?

### 6.4 Error Union vs Exception Model

toke uses error unions (`T!$err`) with explicit propagation (`!$err`) rather than try/catch exceptions. This makes error handling visible in type signatures and forces explicit handling at every call site.

**Question:** Does explicit error propagation increase or decrease LLM generation accuracy compared to exception-based models?

### 6.5 Arena Memory Model

toke uses arena allocation with lexical scoping. No garbage collector, no reference counting, no manual malloc/free. All allocations are tied to arena lifetimes.

**Question:** Is the arena model sufficient for the target use cases, or will common patterns require escape hatches?

### 6.6 Semicolons as Universal Terminators

Every statement, declaration, and field ends with `;`. No optional semicolons, no ASI (automatic semicolon insertion). This adds tokens but eliminates an entire class of ambiguity.

**Question:** Given that semicolons are single-character tokens in most BPE vocabularies, is the consistency worth the token overhead?

### 6.7 12 Keywords vs Readability

toke's keyword set is deliberately minimal: `m`, `f`, `t`, `i`, `if`, `el`, `lp`, `br`, `let`, `mut`, `as`. Single-character declaration keywords prioritise token efficiency over human readability.

**Question:** Does extreme keyword compression help or hurt LLM generation accuracy? Would slightly longer keywords (e.g., `fn` instead of `f`, `mod` instead of `m`) improve model performance enough to justify the extra tokens?

### 6.8 Purpose-Built Tokenizer Strategy

toke trains a custom BPE tokenizer on its own corpus rather than using general-purpose tokenizers (cl100k, Llama tokenizer). This creates a dependency: the model must be fine-tuned with the custom tokenizer, so toke cannot be generated by off-the-shelf models.

**Question:** Is the purpose-built tokenizer dependency acceptable? What is the minimum token reduction that justifies it?

---

## 7. Evaluation Methodology Review

### 7.1 Benchmark Design

1,000 held-out tasks (algorithmic, data-to-code). Each task has:
- Natural language description
- Typed input/output specification
- 20–120 test cases with expected outputs
- Reference implementations in Python, C, and Java

Evaluation: Generate toke code, compile with `tkc`, run against test cases. Pass@1 = fraction of tasks where all test cases pass on first generation attempt.

**Question:** Is 1,000 tasks sufficient for statistical significance? Should we stratify by difficulty or category?

### 7.2 Differential Testing

Every corpus entry is validated by generating equivalent programs in Python, C, and Java, then checking output agreement across languages. Majority agreement (2/3 languages) required.

**Question:** Is majority agreement sufficient, or should we require unanimity? How should we handle floating-point precision differences?

### 7.3 Token Counting Methodology

Token efficiency is measured using OpenAI's cl100k_base tokenizer as baseline. toke programs are compared against equivalent Python/C/Java programs.

**Question:** Is cl100k_base the right baseline? Should we also measure against Llama/Mistral tokenizers for broader applicability?

---

## 8. Open Questions from the Specification

These questions from the RFC (Section 23) remain unresolved:

1. **Generics:** Should toke v0.2 add parametric polymorphism? What syntax minimises token overhead?
2. **Async model:** Spawn/await vs structured concurrency — which is more LLM-friendly?
3. **C FFI:** How much interop surface is needed? Minimal extern declarations or full ABI compatibility?
4. **Module versioning:** Should version constraints appear in source (`i=http:std.http@1.2`) or in a manifest file?
5. **Standard library scope:** 14 modules covers core I/O, data formats, networking, crypto, and i18n. Is this the right scope, or should some modules be deferred?

---

## 9. Project Timeline

| Milestone | Status | Key Result |
|-----------|--------|------------|
| 1.1 Language specification | Complete | LL(1) grammar, toke character set (56 chars), 12 keywords |
| 1.2 Reference compiler | Complete | Single-pass C99, LLVM backend, 90 conformance + 9 e2e tests |
| 1.3 Standard library | Complete | 14 modules, C runtime backing |
| 1.5 Training corpus | Complete | 46,754 validated programs, default syntax |
| 1.6 Gate 1 evaluation | **PASS** | 12.5% token reduction, 63.7% Pass@1 (588/923 tasks) |
| 2.2 Purpose-built tokenizer | Complete | 8K/32K vocab, legacy profile corpus |
| 2.3 Fine-tuned model | Complete | QLoRA on Qwen 2.5 Coder 7B, LoRA adapter |
| 6.3 Serialization formats | Complete | TOON, YAML, JSON modules + i18n (ADR-0003) |

---

## 10. How to Review

### Source Material

| Document | Location |
|----------|----------|
| RFC draft | [spec/rfc/draft-karwalski-toke-lang-00.md](https://github.com/karwalski/toke/blob/main/spec/rfc/draft-karwalski-toke-lang-00.md) |
| Language specification v0.1 | [spec/spec/toke-spec-v02.md](https://github.com/karwalski/toke/blob/main/spec/spec/toke-spec-v02.md) |
| Formal grammar (EBNF) | [spec/spec/grammar.ebnf](https://github.com/karwalski/toke/blob/main/spec/spec/grammar.ebnf) |
| Error code registry | [spec/spec/errors.md](https://github.com/karwalski/toke/blob/main/spec/spec/errors.md) |
| Type system semantics | [spec/spec/semantics.md](https://github.com/karwalski/toke/blob/main/spec/spec/semantics.md) |
| Architecture decisions | [ADR-0001](https://github.com/karwalski/toke/blob/main/spec/docs/architecture/ADR-0001.md), [ADR-0003](https://github.com/karwalski/toke/blob/main/spec/docs/architecture/ADR-0003.md) |
| Gate 1 decision | [gate1-decision.md](https://github.com/karwalski/toke/blob/main/spec/docs/gate1-decision.md) |
| Benchmark methodology | [toke-eval/benchmark/docs/benchmark-design.md](https://github.com/karwalski/toke-eval/blob/main/benchmark/docs/benchmark-design.md) |
| Website | [tokelang.dev](https://tokelang.dev) |

### What We Want

1. **Language design critique** — Are the syntax decisions well-justified? What would you change?
2. **Evaluation methodology review** — Is the benchmark design sound? Are the gate criteria appropriate?
3. **Tokenizer strategy feedback** — Is a purpose-built tokenizer the right approach?
4. **Corpus methodology assessment** — Is differential testing + majority agreement sufficient for training data quality?
5. **Use case assessment** — Beyond the stated use case (LLM code generation), do you see other applications or limitations?

### How to Provide Feedback

- Open issues on [github.com/karwalski/toke](https://github.com/karwalski/toke/issues)
- Comment on the RFC draft via pull request
- Email: toke@karwalski.dev

---

## Appendix A: Complete Grammar (Summary)

```ebnf
SourceFile  = ModuleDecl { ImportDecl } { TypeDecl } { ConstDecl } { FuncDecl } EOF ;
ModuleDecl  = "m" , "=" , Ident , ";" ;
ImportDecl  = "i" , "=" , Ident , ":" , QualPath , ";" ;
TypeDecl    = "t" , "=" , SigilType , "{" , FieldList , "}" , ";" ;
ConstDecl   = "c" , "=" , Ident , [ ":" , TypeExpr ] , Expr , ";" ;
FuncDecl    = "f" , "=" , Ident , "(" , [ ParamList ] , ")" , [ ":" , TypeExpr ] , ( Block | ";" ) ;
```

## Appendix B: Tokenizer Comparison

### Token Reduction vs cl100k_base (Legacy Profile Corpus)

| Metric | 8K Vocabulary | 32K Vocabulary |
|--------|--------------|----------------|
| Token reduction vs cl100k_base | **12.5%** | **13.1%** |
| Mean tokens (toke BPE) | 172.9 | 171.8 |
| Mean tokens (cl100k baseline) | 197.6 | 197.6 |
| Compression ratio | 0.875 | 0.869 |
| Vocabulary utilisation | 70.2% | 23.5% |
| Fertility | 0.377 | 0.374 |

### Cross-Language Token Comparison (cl100k_base, Complete Programs)

| Language | Mean tokens | Ratio to toke |
|----------|------------|---------------|
| **toke** | **52** | 1.0x |
| Java | 127 | 2.4x |
| Python | 156 | 3.0x |
| C | 168 | 3.2x |

## Appendix C: Sample Corpus Entries (Default Syntax)

### Array sum (Phase A — Array, 45 tokens)
```
m=sum;f=sum(arr:@i64):i64{let total=mut.0;lp(let i=0;i<arr.len;i=i+1){total=total+arr.get(i);};<total};
```

### Maximum of two (Phase A — Conditional, 27 tokens)
```
m=max2;f=max2(a:i64;b:i64):i64{if(a>b){<a};<b};
```

### Safe division with error union (Phase A — Error, 60 tokens)
```
m=safediv;t=$matherr{$divbyzero:bool;$overflow:$str};f=safeDiv(a:i64;b:i64):i64!$matherr{if(b=0){<$matherr{$divbyzero:true;$overflow:""};};<a/b};
```

### Addition (Phase A — Math, 19 tokens)
```
m=add;f=add(a:i64;b:i64):i64{<a+b;}
```

### String reversal (Phase A — String, 50 tokens)
```
m=reverse;f=reverse(s:$str):$str{let len=s.len;let result=mut."";lp(let i=len-1;i>-1;i=i-1){result=result+s.get(i as u64);};<result};
```

### Bubble sort (Phase A — Sort, 91 tokens)
```
m=bubblesort;f=bubbleSort(arr:@i64):@i64{let result=mut.arr;let n=arr.len;lp(let x=0;x<n;x=x+1){lp(let y=0;y+1<n;y=y+1){if(result.get(y)>result.get(y+1)){let tmp=result.get(y);result.get(y)=result.get(y+1);result.get(y+1)=tmp;};};};<result};
```

**Key default syntax features visible in these examples:**
- Lowercase keywords: `m=`, `f=`, `t=`, `lp`, `el`
- `$` type sigils: `$str`, `$matherr`, `$divbyzero`
- `@` array types: `@i64`
- `.get()` indexing: `arr.get(i)`
- `<` return operator
- `!$err` error unions: `i64!$matherr`
