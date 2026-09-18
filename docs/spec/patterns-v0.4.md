---
title: toke v0.4 — Pattern catalogue (normative)
slug: patterns-v0.4
section: spec
---

> **GENERATED** by `scripts/patterns/render_catalogue.py spec` from `patterns/catalogue.json` (sha256 `fa57606a3a04`). Do not hand-edit: change the catalogue, run `make render-patterns`; `make check-patterns` fails CI on drift.

**Status:** normative (Epic 131). **Protocol:** `0.4` — [patterns-protocol-v0.4](/docs/spec/patterns-protocol-v0.4/) defines the schema (§3), the token (§4) and runtime (§5) measurements, the verdict algorithm (§6) and how a verdict is enforced (§7). **Companion:** [idiom-v0.4](/docs/spec/idiom-v0.4/) (the prose rules these verdicts measure), [Lesson 11 — Patterns and Efficiency](/docs/learn/11-patterns-and-efficiency/) (the teaching view of the same data).

Every entry lists all measured candidate forms and the verdict that follows from their numbers. The **canonical** form is the default idiom; a **hot path** form exists only where the token-best and runtime-best forms differ, with a written rule for when to choose it. `pending` marks a runtime column not yet ingested from `bench/patterns/results/`; a verdict is `provisional` until re-measured under a tokenizer trained on the rewritten corpus (protocol §8). Runtime verdicts on a `pending` row are the author's expectation and are re-derived on ingest.

## Summary

| id | family | canonical | hot path | status | lint rule |
|---|---|---|---|---|---|
| [`cond-bind-if`](#cond-bind-if) | `cond` | `c` | — | provisional | `mut-flag-if` (warning) |
| [`cond-elif-chain`](#cond-elif-chain) | `cond` | `a` | — | provisional | `mut-flag-if` (warning) |
| [`cond-bool-combine`](#cond-bool-combine) | `cond` | `a` | — | provisional | `flag-soup` (warning) |
| [`cond-clamp`](#cond-clamp) | `cond` | `a` | — | provisional | `mut-flag-if` (warning) |
| [`cond-bool-render`](#cond-bool-render) | `cond` | `a` | — | blocked | `mut-flag-if` (warning) |
| [`acc-sum`](#acc-sum) | `acc` | `a` | — | provisional | — |
| [`acc-count-if`](#acc-count-if) | `acc` | `a` | — | provisional | — |
| [`acc-min-max`](#acc-min-max) | `acc` | `c` | `a` | provisional | — |
| [`acc-array`](#acc-array) | `acc` | `a` | — | provisional | — |
| [`acc-map-build`](#acc-map-build) | `acc` | `b` | — | provisional | — |
| [`acc-dedupe`](#acc-dedupe) | `acc` | `c` | — | blocked | — |
| [`str-build-loop`](#str-build-loop) | `str` | `b` | — | provisional | `string-concat-chain` (warning) |
| [`str-interp-vs-join`](#str-interp-vs-join) | `str` | `a` | — | provisional | `string-concat-chain` (warning) |
| [`str-num-format`](#str-num-format) | `str` | `a` | — | provisional | — |
| [`str-repeat-pad`](#str-repeat-pad) | `str` | `a` | — | provisional | — |
| [`str-array-render`](#str-array-render) | `str` | `c` | — | blocked | — |
| [`err-propagate`](#err-propagate) | `err` | `a` | — | provisional | — |
| [`err-default`](#err-default) | `err` | `a` | — | provisional | `single-use-let` (hint) |
| [`err-validate-early`](#err-validate-early) | `err` | `a` | — | provisional | — |

## Family `cond` — Conditionals

*Intent:* conditional binding and boolean logic.

### cond-bind-if

**Intent.** Bind a value that depends on a condition and use it afterwards.

**Applicability.** Two-way choice whose result feeds a later expression. Form c (return in each branch) only applies when the value is returned immediately and the continuation is tiny (here `*x+g`, 5 bytes); it duplicates the continuation, so with any reused or longer continuation the expression-if bind (a) wins. Form b is the mut-flag anti-pattern (idiom rule 1).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | expression-if bound with let — expected: tied (branch only, no alloc) | 17 | 46 | 17 | 32 | 30 | 46 | pending | pending | pending | pending | tied | more |
| b | mut-flag then if/el assigns — expected: tied | 20 | 56 | 19 | 39 | 37 | 56 | pending | pending | pending | pending | tied | more |
| **c** | statement-if returning from each branch — expected: tied | 15 | 43 | 15 | 31 | 29 | 43 | pending | pending | pending | pending | tied | best |

**Verdict.** canonical = `c` (statement-if returning from each branch); status = `provisional`.

**Canonical form** (`patterns/cond-bind-if/c.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64):i64{
  if(x>0){<1*x+1}el{<2*x+2}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i%5-2)
  };
  io.println("acc=\(acc)");
  <0
};
```

**Form `a`** — expression-if bound with let (`patterns/cond-bind-if/a.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  let g=if(x>0){1}el{2};
  <g*x+g
};
```

**Form `b`** — mut-flag then if/el assigns (`patterns/cond-bind-if/b.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  let g=mut.0;
  if(x>0){g=1}el{g=2};
  <g*x+g
};
```

**Sources.**

- `idiom-v0.4#1`
- `card:NEVER write `let x=mut.0; if(c){x=a}el{x=b}``
- `ast-mine:30`
- `ast-mine:24`

**Lint.** `mut-flag-if` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### cond-elif-chain

**Intent.** Choose one of several values from an ordered set of threshold tests.

**Applicability.** Three or more mutually exclusive conditions tested in order (grades, buckets, tiers). Form c (a ladder of independent ifs overwriting one mut) is only equivalent when later tests subsume earlier ones, so it is fragile as well as verbose.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `el if` chain as one expression — expected: tied | 21 | 63 | 26 | 43 | 38 | 63 | pending | pending | pending | pending | tied | best |
| b | nested if/el expressions — expected: tied | 22 | 65 | 22 | 46 | 41 | 65 | pending | pending | pending | pending | tied | tied |
| c | mut-flag ladder of independent ifs — expected: tied (evaluates every test) | 28 | 74 | 27 | 51 | 46 | 74 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (`el if` chain as one expression); status = `provisional`.

**Canonical form** (`patterns/cond-elif-chain/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64):i64{
  <if(x>90){4}el if(x>80){3}el if(x>70){2}el{1}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i%100)
  };
  io.println("acc=\(acc)");
  <0
};
```

**Form `b`** — nested if/el expressions (`patterns/cond-elif-chain/b.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  <if(x>90){4}el{if(x>80){3}el{if(x>70){2}el{1}}}
};
```

**Form `c`** — mut-flag ladder of independent ifs (`patterns/cond-elif-chain/c.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  let g=mut.1;
  if(x>70){g=2};
  if(x>80){g=3};
  if(x>90){g=4};
  <g
};
```

**Sources.**

- `idiom-v0.4#1`
- `card:expression-if with el if chains`

**Lint.** `mut-flag-if` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### cond-bool-combine

**Intent.** Compute a value from the OR of two tests.

**Applicability.** Any boolean combination; `&&`/`||` short-circuit. Form b (flag soup) evaluates every test; form c (sequential early returns) works only when the result is returned directly.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `\|\|` inside one expression-if — expected: tied (short-circuits) | 10 | 45 | 16 | 32 | 29 | 45 | pending | pending | pending | pending | tied | best |
| b | flag soup: two ifs setting one mut — expected: tied | 17 | 68 | 20 | 43 | 40 | 68 | pending | pending | pending | pending | tied | more |
| c | sequential guard returns — expected: tied | 13 | 50 | 13 | 35 | 32 | 50 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (`\|\|` inside one expression-if); status = `provisional`.

**Canonical form** (`patterns/cond-bool-combine/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(a:i64;b:i64):i64{
  <if(a>0||b>0){1}el{0}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i%3-1;i%7-3)
  };
  io.println("acc=\(acc)");
  <0
};
```

**Form `b`** — flag soup: two ifs setting one mut (`patterns/cond-bool-combine/b.tk`, `pat` only):

```toke
f=pat(a:i64;b:i64):i64{
  let ok=mut.0;
  if(a>0){ok=1};
  if(b>0){ok=1};
  <ok
};
```

**Form `c`** — sequential guard returns (`patterns/cond-bool-combine/c.tk`, `pat` only):

```toke
f=pat(a:i64;b:i64):i64{
  if(a>0){<1};
  if(b>0){<1};
  <0
};
```

**Sources.**

- `idiom-v0.4#2`
- `card:Never simulate OR/AND with if-flags`
- `ast-mine:49`

**Lint.** `flag-soup` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### cond-clamp

**Intent.** Clamp a value into [lo,hi] (min/max).

**Applicability.** Any bounded value; the stdlib has no min/max combinators (ABSENT per combinator-status), so an expression-if chain is the primitive. A helper-function form was not measured because helper bodies fall outside the counted `pat` region.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `el if` chain expression — expected: tied | 11 | 65 | 19 | 41 | 37 | 65 | pending | pending | pending | pending | tied | best |
| b | mut copy then two clamping ifs — expected: tied | 18 | 76 | 22 | 48 | 44 | 76 | pending | pending | pending | pending | tied | more |
| c | two guard returns then value — expected: tied | 17 | 62 | 8 | 41 | 37 | 62 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (`el if` chain expression); status = `provisional`.

**Canonical form** (`patterns/cond-clamp/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64;lo:i64;hi:i64):i64{
  <if(x<lo){lo}el if(x>hi){hi}el{x}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i%200-50;0;100)
  };
  io.println("acc=\(acc)");
  <0
};
```

**Form `b`** — mut copy then two clamping ifs (`patterns/cond-clamp/b.tk`, `pat` only):

```toke
f=pat(x:i64;lo:i64;hi:i64):i64{
  let r=mut.x;
  if(r<lo){r=lo};
  if(r>hi){r=hi};
  <r
};
```

**Form `c`** — two guard returns then value (`patterns/cond-clamp/c.tk`, `pat` only):

```toke
f=pat(x:i64;lo:i64;hi:i64):i64{
  if(x<lo){<lo};
  if(x>hi){<hi};
  <x
};
```

**Sources.**

- `idiom-v0.4#1`
- `card:expression-if with el if chains`

**Lint.** `mut-flag-if` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### cond-bool-render

**Intent.** Render a boolean test as the text `true`/`false`.

**Applicability.** Printing or storing a bool as text. Interpolating a bool prints `1`/`0` on tkc 2.8.0 (127.15), so form b changes the output and is blocked; 8.5% of corpus programs hand-write a boolstr helper for this (131.30).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | expression-if selecting the literal — expected: tied (no alloc: literal returned) | 8 | 41 | 15 | 25 | 24 | 48 | pending | pending | pending | pending | tied | best |
| b | interpolate the bool `"\(c)"` *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |
| c | mut string flag overwritten by an if — expected: tied | 13 | 52 | 17 | 29 | 28 | 59 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (expression-if selecting the literal); status = `blocked`.

**Canonical form** (`patterns/cond-bool-render/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(x:i64):str{
  <if(x%2==0){"true"}el{"false"}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+s.len(pat(i))
  };
  io.println("len=\(acc)");
  <0
};
```

**Form `b`** — interpolate the bool `"\(c)"` — **blocked on 127.15** (`patterns/cond-bool-render/b.blocked.tk`, not compiled):

```text
f=pat(x:i64):str{
  <"\(x%2==0)"
};
```

**Form `c`** — mut string flag overwritten by an if (`patterns/cond-bool-render/c.tk`, `pat` only):

```toke
f=pat(x:i64):str{
  let r=mut."false";
  if(x%2==0){r="true"};
  <r
};
```

**Sources.**

- `ast-mine:24`
- `ast-mine:30`
- `ast-mine:29`

**Bug caveats.**

- [127.15](/docs/progress/): `"\(x%2==0)"` prints 1/0, not true/false — checksum differs (len=1000 vs 4500 at PAT_N=1000) — preferred when fixed: form `b`

**Lint.** `mut-flag-if` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

## Family `acc` — Accumulation

*Intent:* accumulation into a value or collection.

### acc-sum

**Intent.** Sum the elements of an @i64.

**Applicability.** Numeric reduction over an array. The reduce form needs a two-arg helper function (`addf`) declared outside `pat`; its tokens are not counted in the measured region, so the token tie flatters b. `fold` is ABSENT (127.4); `reduce` is the canonical combinator.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | lp accumulating into a mut — expected: best (inlined add, no calls) | 16 | 76 | 17 | 44 | 42 | 76 | pending | pending | pending | pending | best | best |
| b | `xs.reduce(0;&addf)` — expected: slower (indirect call per element through tk_arr_reduce; helper not inlined) | 16 | 39 | 16 | 22 | 20 | 39 | pending | pending | pending | pending | slower | best |

**Verdict.** canonical = `a` (lp accumulating into a mut); status = `provisional`.

**Canonical form** (`patterns/acc-sum/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(xs:@i64):i64{
  let t=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    t=t+xs.get(i)
  };
  <t
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){
    xs=xs.append((i*7919)%1013+1)
  };
  io.println("r=\(pat(xs))");
  <0
};
```

**Form `b`** — `xs.reduce(0;&addf)` (`patterns/acc-sum/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  <xs.reduce(0;&addf)
};
```

**Sources.**

- `idiom-v0.4#7`
- `ast-mine:40`
- `ast-mine:36`

**Bug caveats.**

- [127.4](/docs/progress/): `xs.fold` links against an undefined symbol; `xs.reduce(init;&f)` is the working equivalent and is what form b uses

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### acc-count-if

**Intent.** Count the elements satisfying a predicate.

**Applicability.** Count with a per-element test. Forms b and c need a helper predicate/step function outside `pat` (not counted). b materialises the filtered array just to take its length.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | lp with if and counter — expected: best (no alloc, no calls) | 16 | 88 | 20 | 51 | 49 | 88 | pending | pending | pending | pending | best | tied |
| b | `xs.filter(&p).len` — expected: slower (allocates an N-slot result array + indirect call per element) | 15 | 41 | 16 | 22 | 20 | 41 | pending | pending | pending | pending | slower | best |
| c | `xs.reduce(0;&step)` with expr-if step — expected: slower (indirect call per element) | 16 | 39 | 16 | 22 | 20 | 39 | pending | pending | pending | pending | slower | tied |

**Verdict.** canonical = `a` (lp with if and counter); status = `provisional`.

**Canonical form** (`patterns/acc-count-if/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(xs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%3==0){c=c+1}
  };
  <c
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){
    xs=xs.append((i*7919)%1013+1)
  };
  io.println("r=\(pat(xs))");
  <0
};
```

**Form `b`** — `xs.filter(&p).len` (`patterns/acc-count-if/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  <xs.filter(&div3).len
};
```

**Form `c`** — `xs.reduce(0;&step)` with expr-if step (`patterns/acc-count-if/c.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  <xs.reduce(0;&cnt3)
};
```

**Sources.**

- `idiom-v0.4#7`
- `ast-mine:36`
- `ast-mine:5`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### acc-min-max

**Intent.** Find the minimum (symmetrically maximum) of a non-empty @i64.

**Applicability.** Non-empty arrays only (all forms index element 0). `min`/`max` combinators are ABSENT. Form c sorts a copy (qsort, O(N log N), one N-word allocation) to read element 0; its 4N/N ratio stays within 1.5x of linear so the protocol classes it `slower`, not `worse-bigO` — hence the hot_path rule.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a (hot path) | lp tracking the running minimum — expected: best (O(N), no alloc) | 18 | 99 | 22 | 53 | 51 | 99 | pending | pending | pending | pending | best | more |
| b | `xs.reduce(xs.get(0);&mn)` — expected: slower (indirect call per element) | 18 | 45 | 17 | 24 | 22 | 45 | pending | pending | pending | pending | slower | more |
| **c** | `xs.sort(&cmp).get(0)` — expected: slower (qsort O(N log N) + full copy) | 14 | 41 | 14 | 23 | 21 | 41 | pending | pending | pending | pending | slower | best |

**Verdict.** canonical = `c` (`xs.sort(&cmp).get(0)`); hot path = `a` (lp tracking the running minimum) — choose it when arrays larger than ~1k elements, or the minimum is taken inside a loop body: sort is O(N log N) plus a full copy, the loop is O(N) with no allocation; status = `provisional`.

**Canonical form** (`patterns/acc-min-max/c.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=cmp(a:i64;b:i64):i64{<a-b};
f=pat(xs:@i64):i64{
  <xs.sort(&cmp).get(0)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){
    xs=xs.append((i*7919)%1013+1)
  };
  io.println("r=\(pat(xs))");
  <0
};
```

**Form `a`** (hot path) — lp tracking the running minimum (`patterns/acc-min-max/a.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let m=mut.xs.get(0);
  lp(let i=1;i<xs.len;i=i+1){
    if(xs.get(i)<m){m=xs.get(i)}
  };
  <m
};
```

**Form `b`** — `xs.reduce(xs.get(0);&mn)` (`patterns/acc-min-max/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  <xs.reduce(xs.get(0);&mn)
};
```

**Sources.**

- `idiom-v0.4#7`
- `ast-mine:40`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### acc-array

**Intent.** Accumulate values into a new @i64 in a loop.

**Applicability.** Any loop that collects results. `x=x.append(v)` at a self-update site is lowered to the in-place amortised append (ADR-0006 D2); `x=x+@(v)` calls tk_array_concat, which mallocs and copies the whole array every iteration (O(N^2) bytes, never freed). std.vec is a separate handle type needing `vec.toarray` at the end.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `x=x.append(v)` — expected: best (in-place amortised) | 13 | 74 | 15 | 41 | 39 | 74 | pending | pending | pending | pending | best | best |
| b | `x=x+@(v)` — expected: worse-bigO (concat copies the array each step) | 13 | 69 | 14 | 42 | 40 | 69 | pending | pending | pending | pending | worse-bigO | best |
| c | std.vec push then toarray — expected: tied (amortised push, one final copy) | 27 | 95 | 26 | 47 | 45 | 95 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (`x=x.append(v)`); status = `provisional`.

**Canonical form** (`patterns/acc-array/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(n:i64):@i64{
  let x=mut.@();
  lp(let i=0;i<n;i=i+1){
    x=x.append(i*3)
  };
  <x
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let r=pat(n);
  let sum=mut.0;
  lp(let i=0;i<r.len;i=i+1){
    sum=sum+r.get(i)
  };
  io.println("len=\(r.len) sum=\(sum)");
  <0
};
```

**Form `b`** — `x=x+@(v)` (`patterns/acc-array/b.tk`, `pat` only):

```toke
f=pat(n:i64):@i64{
  let x=mut.@();
  lp(let i=0;i<n;i=i+1){
    x=x+@(i*3)
  };
  <x
};
```

**Form `c`** — std.vec push then toarray (`patterns/acc-array/c.tk`, `pat` only):

```toke
f=pat(n:i64):@i64{
  let v=mut.vec.new();
  lp(let i=0;i<n;i=i+1){
    v=vec.push(v;i*3)
  };
  <vec.toarray(v)
};
```

**Sources.**

- `card:arr=arr.append(v) — PREFERRED accumulator idiom`
- `ast-mine:19`
- `ast-mine:25`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### acc-map-build

**Intent.** Accumulate per-key totals for a small fixed key set.

**Applicability.** Keys drawn from a known set of ~4 strings. Both forms are a linear key scan (the 2.8.0 map is an unsorted entry list searched with strcmp; `m.set` updates in place). Form b only applies when the key set is fixed and small; with an open key set the map is the only correct form. Int-keyed maps crash on 2.8.0, so keys must be str.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | seeded map, `m=m.set(k;m.get(k)+v)` — expected: tied (in-place put, strcmp scan of 4 entries) | 50 | 180 | 50 | 87 | 85 | 180 | pending | pending | pending | pending | tied | tied |
| **b** | parallel arrays + linear key search + `c=c.set(j;…)` — expected: tied (4 string compares, in-place set) | 48 | 209 | 51 | 108 | 106 | 209 | pending | pending | pending | pending | tied | best |

**Verdict.** canonical = `b` (parallel arrays + linear key search + `c=c.set(j;…)`); status = `provisional`.

**Canonical form** (`patterns/acc-map-build/b.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(keys:@str;n:i64):i64{
  let c=mut.@(0;0;0;0);
  lp(let i=0;i<n;i=i+1){
    let k=keys.get(i%4);
    lp(let j=0;j<keys.len;j=j+1){
      if(keys.get(j)==k){c=c.set(j;c.get(j)+i);br}
    }
  };
  <c.get(0)+2*c.get(1)+3*c.get(2)+4*c.get(3)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let keys=@("a";"b";"c";"d");
  io.println("chk=\(pat(keys;n))");
  <0
};
```

**Form `a`** — seeded map, `m=m.set(k;m.get(k)+v)` (`patterns/acc-map-build/a.tk`, `pat` only):

```toke
f=pat(keys:@str;n:i64):i64{
  let m=mut.@("a":0;"b":0;"c":0;"d":0);
  lp(let i=0;i<n;i=i+1){
    let k=keys.get(i%4);
    m=m.set(k;m.get(k)+i)
  };
  <m.get("a")+2*m.get("b")+3*m.get("c")+4*m.get("d")
};
```

**Sources.**

- `card:m2=m2.set(k;v) map write — MUST reassign`
- `ast-mine:32`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### acc-dedupe

**Intent.** Remove duplicate values from an @i64 (order not significant).

**Applicability.** Dedupe where result order is free (checksum is len+sum). Form a (array `.contains`) is the natural form but segfaults on 2.8.0 (127.11). Form d keys a map by the interpolated value: one interpolation alloc per element and a linear strcmp scan of the seen-set, so it is quadratic like b.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | `if(!out.contains(v)){out=out.append(v)}` *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |
| b | inner lp scan with flag+br, then append — expected: worse-bigO (O(N·distinct)) | 25 | 186 | 38 | 89 | 87 | 186 | pending | pending | pending | pending | worse-bigO | best |
| **c** | sort a copy, append when != previous — expected: best (qsort O(N log N) + in-place appends) | 31 | 150 | 38 | 69 | 67 | 150 | pending | pending | pending | pending | best | more |
| d | seen-map keyed by `"\(v)"` — expected: worse-bigO (linear-scan map + 1 alloc per element) | 31 | 176 | 43 | 81 | 79 | 176 | pending | pending | pending | pending | worse-bigO | more |

**Verdict.** canonical = `c` (sort a copy, append when != previous); status = `blocked`.

**Canonical form** (`patterns/acc-dedupe/c.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=cmp(a:i64;b:i64):i64{<a-b};
f=pat(xs:@i64):@i64{
  let ys=xs.sort(&cmp);
  let out=mut.@();
  lp(let i=0;i<ys.len;i=i+1){
    if(i==0||ys.get(i)!=ys.get(i-1)){out=out.append(ys.get(i))}
  };
  <out
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){
    xs=xs.append((i*7919)%(n/4+1))
  };
  let r=pat(xs);
  let sum=mut.0;
  lp(let i=0;i<r.len;i=i+1){
    sum=sum+r.get(i)
  };
  io.println("len=\(r.len) sum=\(sum)");
  <0
};
```

**Form `a`** — `if(!out.contains(v)){out=out.append(v)}` — **blocked on 127.11** (`patterns/acc-dedupe/a.blocked.tk`, not compiled):

```text
f=pat(xs:@i64):@i64{
  let out=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    let v=xs.get(i);
    if(!out.contains(v)){out=out.append(v)}
  };
  <out
};
```

**Form `b`** — inner lp scan with flag+br, then append (`patterns/acc-dedupe/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let out=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    let v=xs.get(i);
    let dup=mut.0;
    lp(let j=0;j<out.len;j=j+1){
      if(out.get(j)==v){dup=1;br}
    };
    if(dup==0){out=out.append(v)}
  };
  <out
};
```

**Form `d`** — seen-map keyed by `"\(v)"` (`patterns/acc-dedupe/d.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let seen=mut.@("":0);
  let out=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    let v=xs.get(i);
    let k="\(v)";
    if(seen.get(k)==0){seen=seen.set(k;1);out=out.append(v)}
  };
  <out
};
```

**Sources.**

- `ast-mine:27`
- `ast-mine:8`
- `ast-mine:43`

**Bug caveats.**

- [127.11](/docs/progress/): array receiver `.contains` is routed to the str glue and SIGSEGVs with a clean check — preferred when fixed: form `a`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

## Family `str` — Strings

*Intent:* building and formatting strings.

### str-build-loop

**Intent.** Build one string from N parts in a loop.

**Applicability.** Any loop that appends text. `s.concat` allocates a fresh copy of the whole accumulator each step (O(N^2) bytes, never freed); the builder grows one buffer; append+join appends in place and allocates once at join.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | `r=s.concat(r;part)` chain — expected: worse-bigO (copies the accumulator per step) | 15 | 96 | 25 | 46 | 45 | 96 | pending | pending | pending | pending | worse-bigO | more |
| **b** | `s.builder` / `s.add` / `s.build` — expected: best (single growing buffer) | 13 | 105 | 28 | 46 | 45 | 105 | pending | pending | pending | pending | best | best |
| c | `acc=acc.append(part)` then `s.join("";acc)` — expected: tied (in-place append + one join alloc) | 15 | 114 | 28 | 49 | 48 | 114 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `b` (`s.builder` / `s.add` / `s.build`); status = `provisional`.

**Canonical form** (`patterns/str-build-loop/b.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(parts:@str;n:i64):str{
  let b=s.builder();
  lp(let i=0;i<n;i=i+1){
    s.add(b;parts.get(i%4))
  };
  <s.build(b)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let parts=@("ab";"cde";"f";"ghij");
  let r=pat(parts;n);
  io.println("len=\(s.len(r)) f=\(s.split(r;"f").len)");
  <0
};
```

**Form `a`** — `r=s.concat(r;part)` chain (`patterns/str-build-loop/a.tk`, `pat` only):

```toke
f=pat(parts:@str;n:i64):str{
  let r=mut."";
  lp(let i=0;i<n;i=i+1){
    r=s.concat(r;parts.get(i%4))
  };
  <r
};
```

**Form `c`** — `acc=acc.append(part)` then `s.join("";acc)` (`patterns/str-build-loop/c.tk`, `pat` only):

```toke
f=pat(parts:@str;n:i64):str{
  let acc=mut.@();
  lp(let i=0;i<n;i=i+1){
    acc=acc.append(parts.get(i%4))
  };
  <s.join("";acc)
};
```

**Sources.**

- `idiom-v0.4#5`
- `ast-mine:13`
- `ast-mine:28`
- `ast-mine:39`

**Lint.** `string-concat-chain` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### str-interp-vs-join

**Intent.** Assemble a 3-5 part string from values of mixed type.

**Applicability.** Templating a fixed number of parts. Interpolation lowers to one tk_str_join_n call (1 alloc); `s.join` needs an array literal plus `s.fromint` for the number (3 allocs); nested `s.concat` allocates per step (5 allocs).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | interpolation `"\(a)-\(b)-\(c)"` — expected: best (1 alloc) | 13 | 47 | 20 | 26 | 25 | 47 | pending | pending | pending | pending | best | best |
| b | `s.join("-";@(a;b;s.fromint(c)))` — expected: slower (array literal + fromint + join) | 15 | 62 | 20 | 28 | 27 | 62 | pending | pending | pending | pending | slower | more |
| c | nested `s.concat` chain — expected: slower (alloc per concat) | 20 | 95 | 28 | 37 | 36 | 95 | pending | pending | pending | pending | slower | more |

**Verdict.** canonical = `a` (interpolation `"\(a)-\(b)-\(c)"`); status = `provisional`.

**Canonical form** (`patterns/str-interp-vs-join/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(a:str;b:str;c:i64):str{
  <"\(a)-\(b)-\(c)"
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+s.len(pat("ab";"cd";i))
  };
  io.println("len=\(acc)");
  <0
};
```

**Form `b`** — `s.join("-";@(a;b;s.fromint(c)))` (`patterns/str-interp-vs-join/b.tk`, `pat` only):

```toke
f=pat(a:str;b:str;c:i64):str{
  <s.join("-";@(a;b;s.fromint(c)))
};
```

**Form `c`** — nested `s.concat` chain (`patterns/str-interp-vs-join/c.tk`, `pat` only):

```toke
f=pat(a:str;b:str;c:i64):str{
  <s.concat(s.concat(s.concat(s.concat(a;"-");b);"-");s.fromint(c))
};
```

**Sources.**

- `idiom-v0.4#5`
- `card:interpolation — PREFERRED for all templating and multi-part strings`
- `ast-mine:13`

**Lint.** `string-concat-chain` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### str-num-format

**Intent.** Convert an i64 to its decimal string.

**Applicability.** Number to text with no width/precision. All three forms reach tk_str_fromi64 (1 alloc). `n as str` is accepted by 2.8.0 although the card lists only numeric casts.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | interpolation `"\(n)"` — expected: tied (1 alloc) | 7 | 25 | 12 | 16 | 15 | 25 | pending | pending | pending | pending | tied | best |
| b | `s.fromint(n)` — expected: tied (1 alloc) | 8 | 31 | 12 | 16 | 15 | 31 | pending | pending | pending | pending | tied | tied |
| c | `n as str` — expected: tied (1 alloc) | 8 | 27 | 11 | 15 | 14 | 27 | pending | pending | pending | pending | tied | tied |

**Verdict.** canonical = `a` (interpolation `"\(n)"`); status = `provisional`.

**Canonical form** (`patterns/str-num-format/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(n:i64):str{
  <"\(n)"
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+s.len(pat(i*7919-1000))
  };
  io.println("len=\(acc)");
  <0
};
```

**Form `b`** — `s.fromint(n)` (`patterns/str-num-format/b.tk`, `pat` only):

```toke
f=pat(n:i64):str{
  <s.fromint(n)
};
```

**Form `c`** — `n as str` (`patterns/str-num-format/c.tk`, `pat` only):

```toke
f=pat(n:i64):str{
  <n as str
};
```

**Sources.**

- `card:io.println("\(value)")`
- `ast-mine:10`
- `ast-mine:2`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### str-repeat-pad

**Intent.** Left-pad a number's text to a fixed width with spaces.

**Applicability.** Fixed-width formatting (tables, ids). Widths are small constants, so every form is a handful of allocations; `s.repeat(str;u64)` exists in 2.8.0 and interpolates correctly. Forms c/d guard `p>0` because `s.repeat` takes a u64.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | lp prepending with `s.concat` — expected: tied (≤w small allocs) | 25 | 97 | 29 | 48 | 46 | 97 | pending | pending | pending | pending | tied | best |
| b | builder: add spaces then the digits — expected: tied | 25 | 126 | 39 | 58 | 56 | 126 | pending | pending | pending | pending | tied | best |
| c | `s.concat(s.repeat(" ";p);d)` — expected: tied (2 allocs) | 28 | 102 | 33 | 49 | 47 | 102 | pending | pending | pending | pending | tied | more |
| d | interpolate `"\(s.repeat(" ";p))\(d)"` — expected: tied (2 allocs) | 28 | 99 | 35 | 51 | 49 | 99 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (lp prepending with `s.concat`); status = `provisional`.

**Canonical form** (`patterns/str-repeat-pad/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(n:i64;w:i64):str{
  let r=mut.s.fromint(n);
  lp(let i=s.len(r);i<w;i=i+1){
    r=s.concat(" ";r)
  };
  <r
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    let r=pat(i%1000;6);
    acc=acc+s.len(r)*10+s.indexof(r;"9")
  };
  io.println("chk=\(acc)");
  <0
};
```

**Form `b`** — builder: add spaces then the digits (`patterns/str-repeat-pad/b.tk`, `pat` only):

```toke
f=pat(n:i64;w:i64):str{
  let b=s.builder();
  let d=s.fromint(n);
  lp(let i=s.len(d);i<w;i=i+1){
    s.add(b;" ")
  };
  s.add(b;d);
  <s.build(b)
};
```

**Form `c`** — `s.concat(s.repeat(" ";p);d)` (`patterns/str-repeat-pad/c.tk`, `pat` only):

```toke
f=pat(n:i64;w:i64):str{
  let d=s.fromint(n);
  let p=w-s.len(d);
  <if(p>0){s.concat(s.repeat(" ";p);d)}el{d}
};
```

**Form `d`** — interpolate `"\(s.repeat(" ";p))\(d)"` (`patterns/str-repeat-pad/d.tk`, `pat` only):

```toke
f=pat(n:i64;w:i64):str{
  let d=s.fromint(n);
  let p=w-s.len(d);
  <if(p>0){"\(s.repeat(" ";p))\(d)"}el{d}
};
```

**Sources.**

- `ast-mine:50`
- `idiom-v0.4#5`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### str-array-render

**Intent.** Collect N labels into a @str, then render each one.

**Applicability.** The collect-then-format shape of CLI/report tasks. Interpolating an element of a `.append`-built `mut.@()` prints an address on 2.8.0 (127.10: the array is never inferred `@str`), so the natural form a is blocked; form b uses the only append form that tags the array (`+@()`) but copies the array every iteration; form c keeps `.append` and reads elements directly into the builder.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | `.append`-built, interpolated read `"[\(labels.get(i))]"` *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |
| b | `+@()`-built, interpolated read — expected: worse-bigO (array concat copies per append) | 24 | 176 | 42 | 75 | 74 | 176 | pending | pending | pending | pending | worse-bigO | tied |
| **c** | `.append`-built, direct `s.add(b;labels.get(i))` reads — expected: best (in-place append, builder) | 23 | 200 | 45 | 82 | 81 | 200 | pending | pending | pending | pending | best | best |

**Verdict.** canonical = `c` (`.append`-built, direct `s.add(b;labels.get(i))` reads); status = `blocked`.

**Canonical form** (`patterns/str-array-render/c.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(n:i64):str{
  let labels=mut.@();
  lp(let i=0;i<n;i=i+1){
    labels=labels.append("l\(i)")
  };
  let b=s.builder();
  lp(let i=0;i<labels.len;i=i+1){
    s.add(b;"[");
    s.add(b;labels.get(i));
    s.add(b;"]")
  };
  <s.build(b)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let r=pat(n);
  io.println("len=\(s.len(r)) n=\(s.split(r;"]").len)");
  <0
};
```

**Form `a`** — `.append`-built, interpolated read `"[\(labels.get(i))]"` — **blocked on 127.10** (`patterns/str-array-render/a.blocked.tk`, not compiled):

```text
f=pat(n:i64):str{
  let labels=mut.@();
  lp(let i=0;i<n;i=i+1){
    labels=labels.append("l\(i)")
  };
  let b=s.builder();
  lp(let i=0;i<labels.len;i=i+1){
    s.add(b;"[\(labels.get(i))]")
  };
  <s.build(b)
};
```

**Form `b`** — `+@()`-built, interpolated read (`patterns/str-array-render/b.tk`, `pat` only):

```toke
f=pat(n:i64):str{
  let labels=mut.@();
  lp(let i=0;i<n;i=i+1){
    labels=labels+@("l\(i)")
  };
  let b=s.builder();
  lp(let i=0;i<labels.len;i=i+1){
    s.add(b;"[\(labels.get(i))]")
  };
  <s.build(b)
};
```

**Sources.**

- `ast-mine:10`
- `ast-mine:15`
- `card:Strings built BY interpolation then stored with arr.append in a loop DANGLE`

**Bug caveats.**

- [127.10](/docs/progress/): `"\(labels.get(i))"` on a `.append`-built array prints pointers (len=12872 vs 5890 at PAT_N=1000); reclassified in 127-disposition-131.md as a missing @str inference, fix in progress — preferred when fixed: form `a`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

## Family `err` — Errors

*Intent:* error unions and early exit.

### err-propagate

**Intent.** Call a T!Err function and propagate its error to the caller.

**Applicability.** Only inside a function that itself returns T!Err (else E3020). Form b re-wraps the error by hand; `!` is the direct form.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `let v=chk(x)!$myerr` — expected: tied | 21 | 49 | 23 | 30 | 28 | 49 | pending | pending | pending | pending | tied | best |
| b | `mt … {$ok:v v;$err:e <$myerr{…}}` re-raise — expected: tied | 24 | 79 | 29 | 42 | 40 | 79 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (`let v=chk(x)!$myerr`); status = `provisional`.

**Canonical form** (`patterns/err-propagate/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
t=$myerr{$bad:bool};
f=chk(x:i64):i64!$myerr{
  if(x%7==0){<$myerr{$bad:true}};
  <x*2
};
f=pat(x:i64):i64!$myerr{
  let v=chk(x)!$myerr;
  <v+1
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    let v=mt pat(i){$ok:v v;$err:e -1};
    acc=acc+v
  };
  io.println("acc=\(acc)");
  <0
};
```

**Form `b`** — `mt … {$ok:v v;$err:e <$myerr{…}}` re-raise (`patterns/err-propagate/b.tk`, `pat` only):

```toke
f=pat(x:i64):i64!$myerr{
  let v=mt chk(x){$ok:v v;$err:e <$myerr{$bad:true}};
  <v+1
};
```

**Sources.**

- `idiom-v0.4#9`
- `card:! propagates — ONLY inside functions that themselves return T!Err`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### err-default

**Intent.** Turn a T!Err result into a plain value with a default on error.

**Applicability.** Consuming an error union where a sentinel is acceptable. Form b avoids the union by re-checking the precondition and calling a non-failing helper (duplicates the check, only possible when the failure condition is known to the caller). Form c is the single-use-let anti-pattern (mined rank 33).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `<mt chk(x){$ok:v v;$err:e -1}` — expected: tied | 11 | 46 | 14 | 27 | 25 | 46 | pending | pending | pending | pending | tied | best |
| b | precondition if + plain call — expected: tied | 12 | 41 | 13 | 26 | 24 | 41 | pending | pending | pending | pending | tied | tied |
| c | `let r=mt …; <r` — expected: tied | 13 | 54 | 16 | 32 | 30 | 54 | pending | pending | pending | pending | tied | more |

**Verdict.** canonical = `a` (`<mt chk(x){$ok:v v;$err:e -1}`); status = `provisional`.

**Canonical form** (`patterns/err-default/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
t=$myerr{$bad:bool};
f=chk(x:i64):i64!$myerr{
  if(x%7==0){<$myerr{$bad:true}};
  <x*2
};
f=raw(x:i64):i64{<x*2};
f=pat(x:i64):i64{
  <mt chk(x){$ok:v v;$err:e -1}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i)
  };
  io.println("acc=\(acc)");
  <0
};
```

**Form `b`** — precondition if + plain call (`patterns/err-default/b.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  if(x%7==0){<-1};
  <raw(x)
};
```

**Form `c`** — `let r=mt …; <r` (`patterns/err-default/c.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  let r=mt chk(x){$ok:v v;$err:e -1};
  <r
};
```

**Sources.**

- `idiom-v0.4#9`
- `ast-mine:33`
- `card:let r=mt safediv(10;2) {$ok:v v;$err:e -1}`

**Lint.** `single-use-let` — severity `hint`, auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.

### err-validate-early

**Intent.** Validate arguments and return sentinels before the main computation.

**Applicability.** Guard clauses at the top of a function. Form b nests each guard in the previous one's else; form c is a single expression-if chain — equal in tokens to a, slightly longer in bytes.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | sequential guard returns `if(..){<-1};` — expected: tied | 13 | 55 | 16 | 36 | 33 | 55 | pending | pending | pending | pending | tied | best |
| b | nested if/el with returns — expected: tied | 15 | 61 | 16 | 40 | 37 | 61 | pending | pending | pending | pending | tied | more |
| c | `<if … el if … el{…}` expression — expected: tied | 13 | 58 | 20 | 39 | 36 | 58 | pending | pending | pending | pending | tied | best |

**Verdict.** canonical = `a` (sequential guard returns `if(..){<-1};`); status = `provisional`.

**Canonical form** (`patterns/err-validate-early/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(a:i64;b:i64):i64{
  if(a<0){<-1};
  if(b==0){<-2};
  <a/b
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+pat(i%13-3;i%5-1)
  };
  io.println("acc=\(acc)");
  <0
};
```

**Form `b`** — nested if/el with returns (`patterns/err-validate-early/b.tk`, `pat` only):

```toke
f=pat(a:i64;b:i64):i64{
  if(a<0){<-1}el{if(b==0){<-2}el{<a/b}}
};
```

**Form `c`** — `<if … el if … el{…}` expression (`patterns/err-validate-early/c.tk`, `pat` only):

```toke
f=pat(a:i64;b:i64):i64{
  <if(a<0){-1}el if(b==0){-2}el{a/b}
};
```

**Sources.**

- `idiom-v0.4#9`
- `card:if(b==0){<$matherr{$divzero:true}}`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `386cf3d11f10`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `deferred-load`; date 2026-09-18.
