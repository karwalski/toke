---
title: toke v0.4 — Pattern catalogue (normative)
slug: patterns-v0.4
section: spec
---

> **GENERATED** by `scripts/patterns/render_catalogue.py spec` from `patterns/catalogue.json` (sha256 `874f0fd4985a`). Do not hand-edit: change the catalogue, run `make render-patterns`; `make check-patterns` fails CI on drift.

**Status:** normative (Epic 131). **Protocol:** `0.4` — [patterns-protocol-v0.4](/docs/spec/patterns-protocol-v0.4/) defines the schema (§3), the token (§4) and runtime (§5) measurements, the verdict algorithm (§6) and how a verdict is enforced (§7). **Companion:** [idiom-v0.4](/docs/spec/idiom-v0.4/) (the prose rules these verdicts measure), [Lesson 11 — Patterns and Efficiency](/docs/learn/11-patterns-and-efficiency/) (the teaching view of the same data).

Every entry lists all measured candidate forms and the verdict that follows from their numbers. The **canonical** form is the default idiom; a **hot path** form exists only where the token-best and runtime-best forms differ, with a written rule for when to choose it. `pending` marks a runtime column not yet ingested from `bench/patterns/results/`; a verdict is `provisional` until re-measured under a tokenizer trained on the rewritten corpus (protocol §8). Runtime verdicts on a `pending` row are the author's expectation and are re-derived on ingest.

## Summary

| id | family | canonical | hot path | status | lint rule |
|---|---|---|---|---|---|
| [`cond-bind-if`](#cond-bind-if) | `cond` | `c` | — | provisional | `mut-flag-if` (warning) |
| [`cond-elif-chain`](#cond-elif-chain) | `cond` | `a` | — | provisional | `mut-flag-if` (warning) |
| [`cond-bool-combine`](#cond-bool-combine) | `cond` | `a` | `c` | provisional | `flag-soup` (warning) |
| [`cond-clamp`](#cond-clamp) | `cond` | `a` | — | provisional | `mut-flag-if` (warning) |
| [`cond-bool-render`](#cond-bool-render) | `cond` | `a` | — | blocked | `mut-flag-if` (warning) |
| [`acc-sum`](#acc-sum) | `acc` | `a` | — | provisional | — |
| [`acc-count-if`](#acc-count-if) | `acc` | `a` | — | provisional | — |
| [`acc-min-max`](#acc-min-max) | `acc` | `c` | `a` | provisional | — |
| [`acc-array`](#acc-array) | `acc` | `a` | — | provisional | — |
| [`acc-map-build`](#acc-map-build) | `acc` | `b` | — | provisional | — |
| [`acc-dedupe`](#acc-dedupe) | `acc` | `b` | `d` | blocked | — |
| [`str-build-loop`](#str-build-loop) | `str` | `b` | — | provisional | `string-concat-chain` (warning) |
| [`str-interp-vs-join`](#str-interp-vs-join) | `str` | `a` | `b` | provisional | `string-concat-chain` (warning) |
| [`str-num-format`](#str-num-format) | `str` | `c` | — | provisional | — |
| [`str-repeat-pad`](#str-repeat-pad) | `str` | `a` | `c` | provisional | — |
| [`str-array-render`](#str-array-render) | `str` | `c` | — | blocked | — |
| [`err-propagate`](#err-propagate) | `err` | `a` | — | provisional | — |
| [`err-default`](#err-default) | `err` | `b` | — | provisional | `single-use-let` (hint) |
| [`err-validate-early`](#err-validate-early) | `err` | `a` | — | provisional | — |
| [`parse-json`](#parse-json) | `parse` | `a` | `c` | provisional | `hand-rolled-parser` (warning) |
| [`parse-csv-line`](#parse-csv-line) | `parse` | `b` | — | provisional | — |
| [`parse-delim-split`](#parse-delim-split) | `parse` | `a` | — | provisional | `hand-rolled-parser` (warning) |
| [`parse-fields`](#parse-fields) | `parse` | `a` | — | provisional | `hand-rolled-parser` (warning) |
| [`parse-int`](#parse-int) | `parse` | `a` | `b` | provisional | `hand-rolled-parser` (warning) |
| [`parse-kv-lines`](#parse-kv-lines) | `parse` | `a` | `b` | provisional | — |
| [`iter-map`](#iter-map) | `iter` | `a` | — | provisional | `loop-is-map` (hint) |
| [`iter-filter`](#iter-filter) | `iter` | `a` | — | provisional | `loop-is-filter` (hint) |
| [`iter-filter-sum`](#iter-filter-sum) | `iter` | `c` | `b` | provisional | — |
| [`iter-find-first`](#iter-find-first) | `iter` | `a` | — | provisional | `scan-without-break` (hint) |
| [`iter-count`](#iter-count) | `iter` | `b` | — | provisional | — |
| [`iter-nested-early-exit`](#iter-nested-early-exit) | `iter` | `a` | — | provisional | `flag-break-is-return` (hint) |
| [`coll-lookup-default`](#coll-lookup-default) | `coll` | `a` | — | provisional | — |
| [`coll-membership`](#coll-membership) | `coll` | `a` | — | blocked | — |
| [`coll-sort-take`](#coll-sort-take) | `coll` | `a` | — | blocked | — |
| [`coll-group-by`](#coll-group-by) | `coll` | `a` | — | provisional | — |
| [`coll-reverse`](#coll-reverse) | `coll` | `a` | — | provisional | `quadratic-prepend` (warning) |
| [`coll-swap`](#coll-swap) | `coll` | `b` | — | provisional | `swap-tmp-let` (hint) |
| [`coll-dedupe`](#coll-dedupe) | `coll` | `a` | — | provisional | `quadratic-dedupe` (warning) |
| [`cli-argv`](#cli-argv) | `cli` | `a` | — | provisional | — |
| [`cli-flag-filter`](#cli-flag-filter) | `cli` | `a` | — | provisional | `loop-is-filter` (hint) |
| [`cli-print-results`](#cli-print-results) | `cli` | `a` | `b` | provisional | `nested-concat` (warning) |
| [`fn-helper-vs-inline`](#fn-helper-vs-inline) | `fn` | `a` | — | provisional | — |
| [`fn-chain-vs-let`](#fn-chain-vs-let) | `fn` | `a` | — | provisional | `single-use-let` (hint) |
| [`fn-recursion-vs-loop`](#fn-recursion-vs-loop) | `fn` | `a` | `b` | provisional | — |
| [`io-read-lines`](#io-read-lines) | `io` | `a` | — | provisional | `hand-rolled-parser` (warning) |
| [`io-write-accumulate`](#io-write-accumulate) | `io` | `a` | — | provisional | `append-in-loop` (warning) |

## Family `cond` — Conditionals

*Intent:* conditional binding and boolean logic.

### cond-bind-if

**Intent.** Bind a value that depends on a condition and use it afterwards.

**Applicability.** Two-way choice whose result feeds a later expression. Form c (return in each branch) only applies when the value is returned immediately and the continuation is tiny (here `*x+g`, 5 bytes); it duplicates the continuation, so with any reused or longer continuation the expression-if bind (a) wins. Form b is the mut-flag anti-pattern (idiom rule 1).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | expression-if bound with let — expected: tied (branch only, no alloc) | 17 | 46 | 17 | 32 | 30 | 46 | 73.91 | 1456 | 199 / 28507 | 3.56 | tied | more |
| b | mut-flag then if/el assigns — expected: tied | 20 | 56 | 19 | 39 | 37 | 56 | 72.88 | 1456 | 199 / 28507 | 3.51 | best | more |
| **c** | statement-if returning from each branch — expected: tied | 15 | 43 | 15 | 31 | 29 | 43 | 75.16 | 1440 | 199 / 28507 | 3.26 | tied | best |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### cond-elif-chain

**Intent.** Choose one of several values from an ordered set of threshold tests.

**Applicability.** Three or more mutually exclusive conditions tested in order (grades, buckets, tiers). Form c (a ladder of independent ifs overwriting one mut) is only equivalent when later tests subsume earlier ones, so it is fragile as well as verbose.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `el if` chain as one expression — expected: tied | 21 | 63 | 26 | 43 | 38 | 63 | 69.30 | 1440 | 199 / 28520 | 3.55 | tied | best |
| b | nested if/el expressions — expected: tied | 22 | 65 | 22 | 46 | 41 | 65 | 66.73 | 1440 | 199 / 28520 | 3.83 | best | tied |
| c | mut-flag ladder of independent ifs — expected: tied (evaluates every test) | 28 | 74 | 27 | 51 | 46 | 74 | 68.91 | 1456 | 199 / 28520 | 3.90 | tied | more |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### cond-bool-combine

**Intent.** Compute a value from the OR of two tests.

**Applicability.** Any boolean combination; `&&`/`||` short-circuit. Form b (flag soup) evaluates every test; form c (sequential early returns) works only when the result is returned directly.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `\|\|` inside one expression-if — expected: tied (short-circuits) | 10 | 45 | 16 | 32 | 29 | 45 | 99.86 | 1456 | 199 / 28527 | 3.56 | slower | best |
| b | flag soup: two ifs setting one mut — expected: tied | 17 | 68 | 20 | 43 | 40 | 68 | 96.27 | 1440 | 199 / 28527 | 3.81 | slower | more |
| c (hot path) | sequential guard returns — expected: tied | 13 | 50 | 13 | 35 | 32 | 50 | 91.48 | 1440 | 199 / 28527 | 3.92 | best | more |

**Verdict.** canonical = `a` (`\|\|` inside one expression-if); hot path = `c` (sequential guard returns) — choose it when the combined result is returned straight out of the function and the test sits in a hot loop: sequential guard returns measured 91.5 ms against 99.9 ms for the single `||` expression (+9.2%), for 3 tokens more; status = `provisional`.

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

**Form `c`** (hot path) — sequential guard returns (`patterns/cond-bool-combine/c.tk`, `pat` only):

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### cond-clamp

**Intent.** Clamp a value into [lo,hi] (min/max).

**Applicability.** Any bounded value; the stdlib has no min/max combinators (ABSENT per combinator-status), so an expression-if chain is the primitive. A helper-function form was not measured because helper bodies fall outside the counted `pat` region.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `el if` chain expression — expected: tied | 11 | 65 | 19 | 41 | 37 | 65 | 59.05 | 1456 | 199 / 28501 | 4.31 | best | best |
| b | mut copy then two clamping ifs — expected: tied | 18 | 76 | 22 | 48 | 44 | 76 | 72.24 | 1456 | 199 / 28501 | 3.58 | slower | more |
| c | two guard returns then value — expected: tied | 17 | 62 | 8 | 41 | 37 | 62 | 64.30 | 1456 | 199 / 28501 | 3.57 | slower | more |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### cond-bool-render

**Intent.** Render a boolean test as the text `true`/`false`.

**Applicability.** Printing or storing a bool as text. Interpolating a bool prints `1`/`0` on tkc 2.8.0 (127.15), so form b changes the output and is blocked; 8.5% of corpus programs hand-write a boolstr helper for this (131.30).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | expression-if selecting the literal — expected: tied (no alloc: literal returned) | 8 | 41 | 15 | 25 | 24 | 48 | 81.06 | 1424 | 199 / 28524 | 3.66 | best | best |
| b | interpolate the bool `"\(c)"` *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |
| c | mut string flag overwritten by an if — expected: tied | 13 | 52 | 17 | 29 | 28 | 59 | 92.72 | 1424 | 199 / 28524 | 3.26 | slower | more |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `acc` — Accumulation

*Intent:* accumulation into a value or collection.

### acc-sum

**Intent.** Sum the elements of an @i64.

**Applicability.** Numeric reduction over an array. The reduce form needs a two-arg helper function (`addf`) declared outside `pat`; its tokens are not counted in the measured region, so the token tie flatters b. `fold` is ABSENT (127.4); `reduce` is the canonical combinator.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | lp accumulating into a mut — expected: best (inlined add, no calls) | 16 | 76 | 17 | 44 | 42 | 76 | 63.22 | 232896 | 225 / 268464559 | 4.01 | best | best |
| b | `xs.reduce(0;&addf)` — expected: slower (indirect call per element through tk_arr_reduce; helper not inlined) | 16 | 39 | 16 | 22 | 20 | 39 | 73.12 | 232896 | 225 / 268464559 | 3.68 | slower | best |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### acc-count-if

**Intent.** Count the elements satisfying a predicate.

**Applicability.** Count with a per-element test. Forms b and c need a helper predicate/step function outside `pat` (not counted). b materialises the filtered array just to take its length.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | lp with if and counter — expected: best (no alloc, no calls) | 16 | 88 | 20 | 51 | 49 | 88 | 71.63 | 232896 | 225 / 268464576 | 3.67 | best | tied |
| b | `xs.filter(&p).len` — expected: slower (allocates an N-slot result array + indirect call per element) | 15 | 41 | 16 | 22 | 20 | 41 | 86.39 | 266192 | 226 / 370864600 | 3.67 | slower | best |
| c | `xs.reduce(0;&step)` with expr-if step — expected: slower (indirect call per element) | 16 | 39 | 16 | 22 | 20 | 39 | 84.66 | 232896 | 225 / 268464576 | 3.69 | slower | tied |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### acc-min-max

**Intent.** Find the minimum (symmetrically maximum) of a non-empty @i64.

**Applicability.** Non-empty arrays only (all forms index element 0). `min`/`max` combinators are ABSENT. Form c sorts a copy (qsort, O(N log N), one N-word allocation) to read element 0; its 4N/N ratio stays within 1.5x of linear so the protocol classes it `slower`, not `worse-bigO` — hence the hot_path rule.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a (hot path) | lp tracking the running minimum — expected: best (O(N), no alloc) | 18 | 99 | 22 | 53 | 51 | 99 | 60.40 | 232880 | 225 / 268464566 | 3.67 | best | more |
| b | `xs.reduce(xs.get(0);&mn)` — expected: slower (indirect call per element) | 18 | 45 | 17 | 24 | 22 | 45 | 68.39 | 232880 | 225 / 268464566 | 3.83 | slower | more |
| **c** | `xs.sort(&cmp).get(0)` — expected: slower (qsort O(N log N) + full copy) | 14 | 41 | 14 | 23 | 21 | 41 | 591.29 | 332928 | 226 / 370864590 | 3.91 | slower | best |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### acc-array

**Intent.** Accumulate values into a new @i64 in a loop.

**Applicability.** Any loop that collects results. `x=x.append(v)` at a self-update site is lowered to the in-place amortised append (ADR-0006 D2); `x=x+@(v)` calls tk_array_concat, which mallocs and copies the whole array every iteration (O(N^2) bytes, never freed). std.vec is a separate handle type needing `vec.toarray` at the end.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `x=x.append(v)` — expected: best (in-place amortised) | 13 | 74 | 15 | 41 | 39 | 74 | 77.43 | 260896 | 228 / 268464642 | 3.95 | best | best |
| b | `x=x+@(v)` — expected: worse-bigO (concat copies the array each step) | 13 | 69 | 14 | 42 | 40 | 69 | 30000.00 | pending | pending | 99.00 | worse-bigO | best |
| c | std.vec push then toarray — expected: tied (amortised push, one final copy) | 27 | 95 | 26 | 47 | 45 | 95 | 83.96 | 259632 | 225 / 399535946 | 3.60 | slower | more |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### acc-map-build

**Intent.** Accumulate per-key totals for a small fixed key set.

**Applicability.** Keys drawn from a known set of ~4 strings. Both forms are a linear key scan (the 2.8.0 map is an unsorted entry list searched with strcmp; `m.set` updates in place). Form b only applies when the key set is fixed and small; with an open key set the map is the only correct form. Int-keyed maps crash on 2.8.0, so keys must be str.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | seeded map, `m=m.set(k;m.get(k)+v)` — expected: tied (in-place put, strcmp scan of 4 entries) | 50 | 180 | 50 | 87 | 85 | 180 | 74.36 | 1424 | 204 / 28877 | 3.74 | slower | tied |
| **b** | parallel arrays + linear key search + `c=c.set(j;…)` — expected: tied (4 string compares, in-place set) | 48 | 209 | 51 | 108 | 106 | 209 | 55.17 | 1440 | 201 / 28629 | 3.65 | best | best |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### acc-dedupe

**Intent.** Remove duplicate values from an @i64 (order not significant).

**Applicability.** Dedupe where result order is free (checksum is len+sum). Form a (array `.contains`) is the natural form but segfaults on 2.8.0 (127.11). Form d keys a map by the interpolated value: one interpolation alloc per element and a linear strcmp scan of the seen-set, so it is quadratic like b.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | `if(!out.contains(v)){out=out.append(v)}` *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |
| **b** | inner lp scan with flag+br, then append — expected: worse-bigO (O(N·distinct)) | 25 | 186 | 38 | 89 | 87 | 186 | 141.66 | 1111664 | 16217 / 1024542442 | 24.99 | slower | best |
| c | sort a copy, append when != previous — expected: best (qsort O(N log N) + in-place appends) | 31 | 150 | 38 | 69 | 67 | 150 | 127.73 | 1111792 | 16218 / 1024670466 | 25.83 | tied | more |
| d (hot path) | seen-map keyed by `"\(v)"` — expected: worse-bigO (linear-scan map + 1 alloc per element) | 31 | 176 | 43 | 81 | 79 | 176 | 127.05 | 1112704 | 48248 / 1025263943 | 26.24 | best | more |

**Verdict.** canonical = `b` (inner lp scan with flag+br, then append); hot path = `d` (seen-map keyed by `"\(v)"`) — choose it when the seen-set scan dominates and a constant factor is worth 6 tokens: the seen-map measured 127.1 ms against 141.7 ms for the inner-loop scan (+11.5%) but allocates 3x as much (48k calls vs 16k) and is just as quadratic (bigO 26.2 vs 25.0) — neither form fixes the class; 127.11 (`.contains`) does; status = `blocked`.

**Canonical form** (`patterns/acc-dedupe/b.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=env:std.env;
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

**Form `c`** — sort a copy, append when != previous (`patterns/acc-dedupe/c.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let ys=xs.sort(&cmp);
  let out=mut.@();
  lp(let i=0;i<ys.len;i=i+1){
    if(i==0||ys.get(i)!=ys.get(i-1)){out=out.append(ys.get(i))}
  };
  <out
};
```

**Form `d`** (hot path) — seen-map keyed by `"\(v)"` (`patterns/acc-dedupe/d.tk`, `pat` only):

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `str` — Strings

*Intent:* building and formatting strings.

### str-build-loop

**Intent.** Build one string from N parts in a loop.

**Applicability.** Any loop that appends text. `s.concat` allocates a fresh copy of the whole accumulator each step (O(N^2) bytes, never freed); the builder grows one buffer; append+join appends in place and allocates once at join.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | `r=s.concat(r;part)` chain — expected: worse-bigO (copies the accumulator per step) | 15 | 96 | 25 | 46 | 45 | 96 | 30000.00 | pending | pending | 99.00 | worse-bigO | more |
| **b** | `s.builder` / `s.add` / `s.build` — expected: best (single growing buffer) | 13 | 105 | 28 | 46 | 45 | 105 | 58.86 | 23792 | 512225 / 30941677 | 2.51 | best | best |
| c | `acc=acc.append(part)` then `s.join("";acc)` — expected: tied (in-place append + one join alloc) | 15 | 114 | 28 | 49 | 48 | 114 | 30000.00 | pending | pending | 99.00 | worse-bigO | more |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### str-interp-vs-join

**Intent.** Assemble a 3-5 part string from values of mixed type.

**Applicability.** Templating a fixed number of parts. Interpolation lowers to one tk_str_join_n call (1 alloc); `s.join` needs an array literal plus `s.fromint` for the number (3 allocs); nested `s.concat` allocates per step (5 allocs).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | interpolation `"\(a)-\(b)-\(c)"` — expected: best (1 alloc) | 13 | 47 | 20 | 26 | 25 | 47 | 96.14 | 45424 | 2400199 / 22717420 | 3.36 | slower | best |
| b (hot path) | `s.join("-";@(a;b;s.fromint(c)))` — expected: slower (array literal + fromint + join) | 15 | 62 | 20 | 28 | 27 | 62 | 73.32 | 39184 | 1200199 / 33917420 | 3.40 | best | more |
| c | nested `s.concat` chain — expected: slower (alloc per concat) | 20 | 95 | 28 | 37 | 36 | 95 | 90.04 | 39136 | 2000199 / 21517420 | 3.08 | slower | more |

**Verdict.** canonical = `a` (interpolation `"\(a)-\(b)-\(c)"`); hot path = `b` (`s.join("-";@(a;b;s.fromint(c)))`) — choose it when the same 3-5 part string is assembled millions of times: `s.join` measured 73.3 ms against 96.1 ms for interpolation (+31%) with half the allocations (1.2M calls vs 2.4M), for 2 tokens more; status = `provisional`.

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

**Form `b`** (hot path) — `s.join("-";@(a;b;s.fromint(c)))` (`patterns/str-interp-vs-join/b.tk`, `pat` only):

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### str-num-format

**Intent.** Convert an i64 to its decimal string.

**Applicability.** Number to text with no width/precision. All three forms reach tk_str_fromi64 (1 alloc). `n as str` is accepted by 2.8.0 although the card lists only numeric casts.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | interpolation `"\(n)"` — expected: tied (1 alloc) | 7 | 25 | 12 | 16 | 15 | 25 | 109.55 | 48544 | 2000199 / 34888203 | 3.50 | slower | best |
| b | `s.fromint(n)` — expected: tied (1 alloc) | 8 | 31 | 12 | 16 | 15 | 31 | 86.50 | 32848 | 1000199 / 24028514 | 3.40 | tied | tied |
| **c** | `n as str` — expected: tied (1 alloc) | 8 | 27 | 11 | 15 | 14 | 27 | 83.95 | 32832 | 1000199 / 24028514 | 3.43 | best | tied |

**Verdict.** canonical = `c` (`n as str`); status = `provisional`.

**Canonical form** (`patterns/str-num-format/c.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(n:i64):str{
  <n as str
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

**Form `a`** — interpolation `"\(n)"` (`patterns/str-num-format/a.tk`, `pat` only):

```toke
f=pat(n:i64):str{
  <"\(n)"
};
```

**Form `b`** — `s.fromint(n)` (`patterns/str-num-format/b.tk`, `pat` only):

```toke
f=pat(n:i64):str{
  <s.fromint(n)
};
```

**Sources.**

- `card:io.println("\(value)")`
- `ast-mine:10`
- `ast-mine:2`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### str-repeat-pad

**Intent.** Left-pad a number's text to a fixed width with spaces.

**Applicability.** Fixed-width formatting (tables, ids). Widths are small constants, so every form is a handful of allocations; `s.repeat(str;u64)` exists in 2.8.0 and interpolates correctly. Forms c/d guard `p>0` because `s.repeat` takes a u64.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | lp prepending with `s.concat` — expected: tied (≤w small allocs) | 25 | 97 | 29 | 48 | 46 | 97 | 78.64 | 33552 | 1644199 / 17000515 | 3.74 | slower | best |
| b | builder: add spaces then the digits — expected: tied | 25 | 126 | 39 | 58 | 56 | 126 | 70.61 | 39168 | 1200199 / 44828515 | 3.65 | slower | best |
| c (hot path) | `s.concat(s.repeat(" ";p);d)` — expected: tied (2 allocs) | 28 | 102 | 33 | 49 | 47 | 102 | 67.85 | 26592 | 1200199 / 14072515 | 3.55 | best | more |
| d | interpolate `"\(s.repeat(" ";p))\(d)"` — expected: tied (2 allocs) | 28 | 99 | 35 | 51 | 49 | 99 | 88.78 | 39168 | 2000199 / 17360515 | 3.46 | slower | more |

**Verdict.** canonical = `a` (lp prepending with `s.concat`); hot path = `c` (`s.concat(s.repeat(" ";p);d)`) — choose it when every row of a large table is padded: `s.concat(s.repeat(" ";p);d)` measured 67.8 ms against 78.6 ms for the prepend loop (+16%) and 26.6 MB peak RSS against 32.8 MB, for 3 tokens more; status = `provisional`.

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

**Form `c`** (hot path) — `s.concat(s.repeat(" ";p);d)` (`patterns/str-repeat-pad/c.tk`, `pat` only):

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### str-array-render

**Intent.** Collect N labels into a @str, then render each one.

**Applicability.** The collect-then-format shape of CLI/report tasks. Interpolating an element of a `.append`-built `mut.@()` prints an address on 2.8.0 (127.10: the array is never inferred `@str`), so the natural form a is blocked; form b uses the only append form that tags the array (`+@()`) but copies the array every iteration; form c keeps `.append` and reads elements directly into the builder.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | `.append`-built, interpolated read `"[\(labels.get(i))]"` *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |
| b | `+@()`-built, interpolated read — expected: worse-bigO (array concat copies per append) | 24 | 176 | 42 | 75 | 74 | 176 | 30000.00 | pending | pending | 99.00 | worse-bigO | tied |
| **c** | `.append`-built, direct `s.add(b;labels.get(i))` reads — expected: best (in-place append, builder) | 23 | 200 | 45 | 82 | 81 | 200 | 51.34 | 14896 | 384240 / 11387887 | 1.38 | best | best |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `err` — Errors

*Intent:* error unions and early exit.

### err-propagate

**Intent.** Call a T!Err function and propagate its error to the caller.

**Applicability.** Only inside a function that itself returns T!Err (else E3020). Form b re-wraps the error by hand; `!` is the direct form.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | `let v=chk(x)!$myerr` — expected: tied | 21 | 49 | 23 | 30 | 28 | 49 | 80.98 | 73184 | 4571628 / 73171382 | 3.54 | best | best |
| b | `mt … {$ok:v v;$err:e <$myerr{…}}` re-raise — expected: tied | 24 | 79 | 29 | 42 | 40 | 79 | 85.22 | 73184 | 4571628 / 73171382 | 3.70 | slower | more |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### err-default

**Intent.** Turn a T!Err result into a plain value with a default on error.

**Applicability.** Consuming an error union where a sentinel is acceptable. Form b avoids the union by re-checking the precondition and calling a non-failing helper (duplicates the check, only possible when the failure condition is known to the caller). Form c is the single-use-let anti-pattern (mined rank 33).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | `<mt chk(x){$ok:v v;$err:e -1}` — expected: tied | 11 | 46 | 14 | 27 | 25 | 46 | 149.74 | 144880 | 9143057 / 146314239 | 3.91 | slower | best |
| **b** | precondition if + plain call — expected: tied | 12 | 41 | 13 | 26 | 24 | 41 | 87.91 | 1456 | 199 / 28511 | 3.42 | best | tied |
| c | `let r=mt …; <r` — expected: tied | 13 | 54 | 16 | 32 | 30 | 54 | 149.16 | 144896 | 9143057 / 146314239 | 3.92 | slower | more |

**Verdict.** canonical = `b` (precondition if + plain call); status = `provisional`.

**Canonical form** (`patterns/err-default/b.tk`, full fixture program):

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
  if(x%7==0){<-1};
  <raw(x)
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

**Form `a`** — `<mt chk(x){$ok:v v;$err:e -1}` (`patterns/err-default/a.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  <mt chk(x){$ok:v v;$err:e -1}
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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### err-validate-early

**Intent.** Validate arguments and return sentinels before the main computation.

**Applicability.** Guard clauses at the top of a function. Form b nests each guard in the previous one's else; form c is a single expression-if chain — equal in tokens to a, slightly longer in bytes.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | sequential guard returns `if(..){<-1};` — expected: tied | 13 | 55 | 16 | 36 | 33 | 55 | 57.56 | 1456 | 199 / 28531 | 3.09 | tied | best |
| b | nested if/el with returns — expected: tied | 15 | 61 | 16 | 40 | 37 | 61 | 55.35 | 1456 | 199 / 28531 | 3.41 | best | more |
| c | `<if … el if … el{…}` expression — expected: tied | 13 | 58 | 20 | 39 | 36 | 58 | 58.02 | 1456 | 199 / 28531 | 3.02 | tied | best |

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

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `parse` — Parsing

*Intent:* turning text into values.

### parse-json

**Intent.** Read typed fields (i64, str) out of a JSON document.

**Applicability.** Any structured JSON input. json.dec + typed accessors is the only form that is correct on reordered keys, whitespace, escapes and nesting; the hand-scan forms are sketches that only work on a fixed layout. Runtime gap: json.dec allocates a heap Json per decode; json.i64 returns the 0 sentinel as $err (a field whose value is 0 is indistinguishable from a missing field); json.arr always returns $err on 2.8.0 (131.30 gap).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | json.dec + json.i64/json.str via mt | 40 | 161 | 43 | 76 | 74 | 161 | 66.62 | 27712 | 1024199 / 16608079 | 3.03 | slower | best |
| b | indexof/slice hand scan (sketch) | 46 | 186 | 70 | 77 | 76 | 198 | 58.85 | 27632 | 1024199 / 16240974 | 3.31 | slower | more |
| c (hot path) | per-char charcode scan loop (sketch) | 51 | 244 | 62 | 131 | 125 | 244 | 50.61 | 23616 | 768199 / 15072079 | 3.33 | best | more |

**Verdict.** canonical = `a` (json.dec + json.i64/json.str via mt); hot path = `c` (per-char charcode scan loop (sketch)) — choose it when the layout is fixed and machine-generated, the parse sits in a loop body executed > 100k times AND profiling shows `json.dec` on the hot path; never for external input (hand scans are wrong on reordered or escaped JSON). The charcode scan measured 50.6 ms against 66.6 ms for `json.dec` (-24%), for 11 tokens more; status = `provisional`.

**Canonical form** (`patterns/parse-json/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=json:std.json;
f=pat(src:str):i64{
  let d=mt json.dec(src) {$ok:v v;$err:e <-1};
  let a=mt json.i64(d;"a") {$ok:v v;$err:e <-1};
  let b=mt json.str(d;"b") {$ok:v v;$err:e <-1};
  <a+s.len(b)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){
    t=t+pat("{\"a\":\(i+1),\"b\":\"x\(i%7)\"}")
  };
  io.println("t=\(t)");
  <0
};
```

**Form `b`** — indexof/slice hand scan (sketch) (`patterns/parse-json/b.tk`, `pat` only):

```toke
f=pat(src:str):i64{
  let pa=s.indexof(src;"\"a\":")+4;
  let a=mt s.toint(s.slice(src;pa;s.indexof(src;","))) {$ok:v v;$err:e <-1};
  let pb=s.indexof(src;"\"b\":\"")+5;
  <a+s.len(s.slice(src;pb;s.len(src)-2))
};
```

**Form `c`** (hot path) — per-char charcode scan loop (sketch) (`patterns/parse-json/c.tk`, `pat` only):

```toke
f=pat(src:str):i64{
  let n=s.len(src);
  let a=mut.0;
  let i=mut.5;
  lp(let k=0;k<n;k=k+1){
    let c=s.charcode(src;i);
    if(c<48||c>57){br};
    a=a*10+c-48;
    i=i+1
  };
  let j=mut.i+6;
  let bl=mut.0;
  lp(let k=0;k<n;k=k+1){
    if(s.charcode(src;j)==34){br};
    bl=bl+1;
    j=j+1
  };
  <a+bl
};
```

**Sources.**

- `idiom-v0.4#4`
- `card:Std modules`

**Lint.** `hand-rolled-parser` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### parse-csv-line

**Intent.** Split CSV text into rows of fields.

**Applicability.** b (s.split on newline then comma) is only correct for unquoted fields; a (csv.parse) is required whenever fields may be quoted or contain separators/newlines (RFC 4180). csv.parse takes [byte] so the input needs s.bytes(txt); rows are csvrow structs, read fields via rows.get(r).fields. Interpolating a csv field element prints a pointer (127.7 class) - print it directly or use s.len.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | csv.parse(s.bytes(txt)) rows .fields | 54 | 209 | 59 | 95 | 92 | 209 | 73.54 | 97008 | 2560235 / 102491453 | 3.54 | slower | more |
| **b** | s.split lines then s.split "," | 50 | 199 | 62 | 93 | 90 | 200 | 70.36 | 61792 | 2304219 / 44439092 | 3.57 | best | best |

**Verdict.** canonical = `b` (s.split lines then s.split ","); status = `provisional`.

**Canonical form** (`patterns/parse-csv-line/b.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=csv:std.csv;
f=pat(txt:str):i64{
  let ls=s.split(txt;"\n");
  let t=mut.0;
  lp(let r=0;r<ls.len;r=r+1){
    if(ls.get(r)!=""){
      let fs=s.split(ls.get(r);",");
      t=t+fs.len*100;
      lp(let j=0;j<fs.len;j=j+1){t=t+s.len(fs.get(j))}
    }
  };
  <t
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let b=s.builder();
  lp(let i=0;i<n;i=i+1){s.add(b;"aa,bbb,c\(i%10)\n")};
  io.println("t=\(pat(s.build(b)))");
  <0
};
```

**Form `a`** — csv.parse(s.bytes(txt)) rows .fields (`patterns/parse-csv-line/a.tk`, `pat` only):

```toke
f=pat(txt:str):i64{
  let rows=mt csv.parse(s.bytes(txt)) {$ok:v v;$err:e <-1};
  let t=mut.0;
  lp(let r=0;r<rows.len;r=r+1){
    let fs=rows.get(r).fields;
    t=t+fs.len*100;
    lp(let j=0;j<fs.len;j=j+1){t=t+s.len(fs.get(j))}
  };
  <t
};
```

**Sources.**

- `idiom-v0.4#4`
- `card:Std modules`

**Bug caveats.**

- [127.7](/docs/progress/): interpolating rows.get(r).fields.get(j) prints a pointer (untagged @str from a module call); checksum uses s.len only

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### parse-delim-split

**Intent.** Split one line on a single-character delimiter into a field array.

**Applicability.** Any delimiter split where empty fields must be preserved. s.split is 5-7x fewer tokens than either manual form and is the only one that does not re-scan the string; b re-slices the remainder every iteration (quadratic in fields x line length).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | s.split(line;",") | 7 | 40 | 11 | 16 | 16 | 40 | 83.34 | 57856 | 2304199 / 40143196 | 3.70 | best | best |
| b | indexof + slice loop | 50 | 219 | 55 | 93 | 93 | 219 | 111.52 | 94000 | 3840199 / 66433866 | 3.56 | slower | more |
| c | charcode scan + slice loop | 35 | 185 | 44 | 76 | 75 | 185 | 107.93 | 94064 | 3072199 / 70863196 | 3.71 | slower | more |

**Verdict.** canonical = `a` (s.split(line;",")); status = `provisional`.

**Canonical form** (`patterns/parse-delim-split/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(line:str):@str{
  <s.split(line;",")
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){
    let fs=pat("ab,cde,\(i),f");
    t=t+fs.len*100;
    lp(let j=0;j<fs.len;j=j+1){t=t+s.len(fs.get(j))}
  };
  io.println("t=\(t)");
  <0
};
```

**Form `b`** — indexof + slice loop (`patterns/parse-delim-split/b.tk`, `pat` only):

```toke
f=pat(line:str):@str{
  let r=mut.@();
  let st=mut.0;
  let n=s.len(line);
  lp(let k=0;k<n;k=k+1){
    let p=s.indexof(s.slice(line;st;n);",");
    if(p<0){r=r.append(s.slice(line;st;n));br};
    r=r.append(s.slice(line;st;st+p));
    st=st+p+1
  };
  <r
};
```

**Form `c`** — charcode scan + slice loop (`patterns/parse-delim-split/c.tk`, `pat` only):

```toke
f=pat(line:str):@str{
  let r=mut.@();
  let st=mut.0;
  let n=s.len(line);
  lp(let i=0;i<n;i=i+1){
    if(s.charcode(line;i)==44){r=r.append(s.slice(line;st;i));st=i+1}
  };
  <r.append(s.slice(line;st;n))
};
```

**Sources.**

- `idiom-v0.4#4`
- `card:Strings`

**Lint.** `hand-rolled-parser` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### parse-fields

**Intent.** Split a line on whitespace runs, dropping empty fields.

**Applicability.** Whitespace tokenising (A6). s.fields is canonical; b (split on " " + skip empties) is what LLMs write when they forget fields exists. Direct reads of fields elements are correct; interpolating an element prints a pointer until 127.9 lands - use io.println(x) or s.len.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | s.fields(line) | 9 | 37 | 11 | 14 | 14 | 37 | 79.25 | 53808 | 2048199 / 32975176 | 3.56 | best | best |
| b | s.split(line;" ") + skip-empty loop | 22 | 132 | 34 | 55 | 55 | 132 | 153.06 | 154512 | 4864199 / 106191176 | 3.83 | slower | more |
| c | charcode scan + slice loop | 49 | 230 | 58 | 100 | 99 | 230 | 134.46 | 94080 | 3072199 / 69839176 | 3.47 | slower | more |

**Verdict.** canonical = `a` (s.fields(line)); status = `provisional`.

**Canonical form** (`patterns/parse-fields/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(line:str):@str{
  <s.fields(line)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){
    let fs=pat("  ab \(i)   cde  f ");
    t=t+fs.len*100;
    lp(let j=0;j<fs.len;j=j+1){t=t+s.len(fs.get(j))}
  };
  io.println("t=\(t)");
  <0
};
```

**Form `b`** — s.split(line;" ") + skip-empty loop (`patterns/parse-fields/b.tk`, `pat` only):

```toke
f=pat(line:str):@str{
  let ws=s.split(line;" ");
  let r=mut.@();
  lp(let j=0;j<ws.len;j=j+1){
    if(ws.get(j)!=""){r=r.append(ws.get(j))}
  };
  <r
};
```

**Form `c`** — charcode scan + slice loop (`patterns/parse-fields/c.tk`, `pat` only):

```toke
f=pat(line:str):@str{
  let r=mut.@();
  let st=mut.-1;
  let n=s.len(line);
  lp(let i=0;i<n;i=i+1){
    if(s.charcode(line;i)==32){
      if(st>=0){r=r.append(s.slice(line;st;i));st=-1}
    }el{if(st<0){st=i}}
  };
  <if(st>=0){r.append(s.slice(line;st;n))}el{r}
};
```

**Sources.**

- `idiom-v0.4#4`
- `card:Strings`

**Bug caveats.**

- [127.9](/docs/progress/): "\(s.fields(x).get(i))" prints a pointer; fixture checksums use s.len only

**Lint.** `hand-rolled-parser` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### parse-int

**Intent.** Parse a decimal integer string, yielding a default on failure.

**Applicability.** Any int parse. mt s.toint is 9 tokens vs 25 for a digit loop and handles sign/overflow. Note s.toint result 0 is indistinguishable from $err under mt on 2.8.0 (0 sentinel) when the text is "0".

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | mt s.toint(txt) {$ok:v v;$err:e -1} | 9 | 54 | 20 | 27 | 26 | 54 | 77.35 | 25616 | 1024199 / 16142654 | 3.06 | slower | best |
| b (hot path) | charcode digit loop | 25 | 144 | 41 | 78 | 73 | 144 | 72.73 | 25600 | 1024199 / 16142654 | 3.02 | best | more |

**Verdict.** canonical = `a` (mt s.toint(txt) {$ok:v v;$err:e -1}); hot path = `b` (charcode digit loop) — choose it when millions of fields are parsed and the text is known to be unsigned ASCII digits: the charcode loop measured 72.7 ms against 77.4 ms (+6.4%) — 16 tokens more for 6%, and it handles neither sign nor overflow; status = `provisional`.

**Canonical form** (`patterns/parse-int/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(txt:str):i64{
  <mt s.toint(txt) {$ok:v v;$err:e -1}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+pat("\(i*7)")+pat("x\(i)")};
  io.println("t=\(t)");
  <0
};
```

**Form `b`** (hot path) — charcode digit loop (`patterns/parse-int/b.tk`, `pat` only):

```toke
f=pat(txt:str):i64{
  let n=s.len(txt);
  if(n==0){<-1};
  let v=mut.0;
  lp(let i=0;i<n;i=i+1){
    let c=s.charcode(txt;i);
    if(c<48||c>57){<-1};
    v=v*10+c-48
  };
  <v
};
```

**Sources.**

- `idiom-v0.4#4`
- `idiom-v0.4#9`
- `card:Match expression`

**Lint.** `hand-rolled-parser` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### parse-kv-lines

**Intent.** Turn key=value lines into a str->str map.

**Applicability.** Config-style text. Both forms need a seeded map literal (mut.@() as a map crashes with RT003 on set - 131.30 gap) so the sentinel key "" is present; keys() on a map returned from a user function fails to link (undefined _keys), so the checksum is computed in a helper that receives the map as a typed parameter. String values read back from the map interpolate as pointers (127.7 class); use io.println(v) or s.len(v).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | s.split lines -> s.split "=" -> m.set | 33 | 172 | 54 | 74 | 73 | 173 | 74.75 | 48640 | 1408269 / 42635471 | 4.08 | slower | best |
| b (hot path) | s.indexof "=" + s.slice -> m.set | 45 | 194 | 60 | 86 | 85 | 195 | 67.25 | 40736 | 1152266 / 35467430 | 4.04 | best | more |

**Verdict.** canonical = `a` (s.split lines -> s.split "=" -> m.set); hot path = `b` (s.indexof "=" + s.slice -> m.set) — choose it when millions of lines are parsed: `b` allocates two strings per line where `a` allocates a 2-element array plus two strings, and measured 67.3 ms against 74.7 ms (-10%), for 12 tokens more; status = `provisional`.

**Canonical form** (`patterns/parse-kv-lines/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=chk(m:@(str:str)):i64{
  let ks=m.keys();
  let t=mut.ks.len*1000000;
  lp(let i=0;i<ks.len;i=i+1){t=t+s.len(m.get(ks.get(i)))};
  <t
};
f=pat(txt:str):i64{
  let m=mut.@("":"");
  let ls=s.split(txt;"\n");
  lp(let i=0;i<ls.len;i=i+1){
    let kv=s.split(ls.get(i);"=");
    if(kv.len==2){m=m.set(kv.get(0);kv.get(1))}
  };
  <chk(m)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let b=s.builder();
  lp(let i=0;i<n;i=i+1){s.add(b;"key\(i)=val\(i*3)\n")};
  io.println("t=\(pat(s.build(b)))");
  <0
};
```

**Form `b`** (hot path) — s.indexof "=" + s.slice -> m.set (`patterns/parse-kv-lines/b.tk`, `pat` only):

```toke
f=pat(txt:str):i64{
  let m=mut.@("":"");
  let ls=s.split(txt;"\n");
  lp(let i=0;i<ls.len;i=i+1){
    let l=ls.get(i);
    let p=s.indexof(l;"=");
    if(p>0){m=m.set(s.slice(l;0;p);s.slice(l;p+1;s.len(l)))}
  };
  <chk(m)
};
```

**Sources.**

- `idiom-v0.4#4`
- `idiom-v0.4#6`

**Bug caveats.**

- [127.7](/docs/progress/): "\(m.get(k))" on a str-valued map prints a pointer

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `iter` — Iteration

*Intent:* traversals.

### iter-map

**Intent.** Transform every element of an array with a per-element function.

**Applicability.** Pure per-element transforms with a named function (&f). Keep a lp when the body is stateful or needs the index.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | xs.map(&dbl) | 13 | 34 | 14 | 20 | 18 | 34 | 52.65 | 293136 | 8204 / 256316590 | 13.99 | tied | best |
| b | index loop + append | 16 | 89 | 19 | 46 | 44 | 89 | 51.88 | 293248 | 8218 / 256383990 | 12.75 | best | more |

**Verdict.** canonical = `a` (xs.map(&dbl)); status = `provisional`.

**Canonical form** (`patterns/iter-map/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=dbl(x:i64):i64{<x*2};
f=pat(xs:@i64):@i64{
  <xs.map(&dbl)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  let r=pat(xs);
  let t=mut.0;
  lp(let i=0;i<r.len;i=i+1){t=t+r.get(i)};
  io.println("len=\(r.len) t=\(t)");
  <0
};
```

**Form `b`** — index loop + append (`patterns/iter-map/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let r=mut.@();
  lp(let i=0;i<xs.len;i=i+1){r=r.append(xs.get(i)*2)};
  <r
};
```

**Sources.**

- `idiom-v0.4#7`
- `card:Arrays and maps`
- `ast-mine:1`

**Lint.** `loop-is-map` — severity `hint`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### iter-filter

**Intent.** Keep the elements that satisfy a predicate.

**Applicability.** Predicate is a named bool function. Element interpolation of the result: safe for @i64; for @str the filter result is untagged (127.10 class) - read elements directly.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | xs.filter(&isev) | 14 | 38 | 15 | 21 | 19 | 38 | 154.77 | 1111632 | 16204 / 1024604602 | 23.50 | tied | best |
| b | index loop + if + append | 18 | 107 | 23 | 54 | 52 | 107 | 148.83 | 1111744 | 16218 / 1024608002 | 24.09 | best | more |

**Verdict.** canonical = `a` (xs.filter(&isev)); status = `provisional`.

**Canonical form** (`patterns/iter-filter/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isev(x:i64):bool{<x%2==0};
f=pat(xs:@i64):@i64{
  <xs.filter(&isev)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  let r=pat(xs);
  let t=mut.0;
  lp(let i=0;i<r.len;i=i+1){t=t+r.get(i)};
  io.println("len=\(r.len) t=\(t)");
  <0
};
```

**Form `b`** — index loop + if + append (`patterns/iter-filter/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let r=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%2==0){r=r.append(xs.get(i))}
  };
  <r
};
```

**Sources.**

- `idiom-v0.4#7`
- `card:Arrays and maps`

**Lint.** `loop-is-filter` — severity `hint`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### iter-filter-sum

**Intent.** Sum the elements that satisfy a predicate.

**Applicability.** Filter-then-aggregate. c folds the predicate into a single reduce (one pass, no intermediate array); a allocates the filtered array; b is the inline single loop. Likely hot-path split: b has no indirect call per element.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | xs.filter(&p).reduce(0;&add) | 21 | 52 | 21 | 26 | 24 | 52 | 73.50 | 163360 | 225 / 199782863 | 3.48 | slower | more |
| b (hot path) | single loop with if | 19 | 96 | 22 | 53 | 51 | 96 | 54.70 | 131344 | 224 / 134246839 | 3.53 | best | more |
| **c** | xs.reduce(0;&addev) (predicate folded) | 17 | 40 | 16 | 22 | 20 | 40 | 63.53 | 131328 | 224 / 134246839 | 3.69 | slower | best |

**Verdict.** canonical = `c` (xs.reduce(0;&addev) (predicate folded)); hot path = `b` (single loop with if) — choose it when arrays run past ~100k elements or the loop body is executed > 1k times: the inline loop avoids the per-element indirect call of `reduce` and measured 54.7 ms against 63.5 ms (-14%), for 2 tokens more; status = `provisional`.

**Canonical form** (`patterns/iter-filter-sum/c.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isev(x:i64):bool{<x%2==0};
f=add(a:i64;b:i64):i64{<a+b};
f=addev(a:i64;x:i64):i64{<if(x%2==0){a+x}el{a}};
f=pat(xs:@i64):i64{
  <xs.reduce(0;&addev)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

**Form `a`** — xs.filter(&p).reduce(0;&add) (`patterns/iter-filter-sum/a.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  <xs.filter(&isev).reduce(0;&add)
};
```

**Form `b`** (hot path) — single loop with if (`patterns/iter-filter-sum/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let t=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%2==0){t=t+xs.get(i)}
  };
  <t
};
```

**Sources.**

- `idiom-v0.4#7`
- `card:Arrays and maps`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### iter-find-first

**Intent.** Return the first element satisfying a predicate, or a default.

**Applicability.** Early-exit search. a returns directly from inside the loop (no flag, no br); b scans everything and allocates; c is the mut-flag scan. xs.find(v) is by value, not predicate, and segfaults on arrays (127.11) so it is not a candidate here (see coll-membership).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | loop with direct < return | 17 | 81 | 20 | 45 | 41 | 81 | 82.45 | 260896 | 225 / 268464584 | 3.83 | best | best |
| b | filter(&p) then .get(0) guarded by .len | 22 | 79 | 25 | 38 | 36 | 79 | 107.67 | 262096 | 226 / 399536608 | 3.85 | slower | more |
| c | mut result + full scan | 22 | 99 | 23 | 56 | 52 | 99 | 90.27 | 260912 | 225 / 268464584 | 3.72 | slower | more |

**Verdict.** canonical = `a` (loop with direct < return); status = `provisional`.

**Canonical form** (`patterns/iter-find-first/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=big(x:i64):bool{<x>990};
f=pat(xs:@i64):i64{
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)>990){<xs.get(i)}
  };
  <-1
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

**Form `b`** — filter(&p) then .get(0) guarded by .len (`patterns/iter-find-first/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let hits=xs.filter(&big);
  <if(hits.len>0){hits.get(0)}el{-1}
};
```

**Form `c`** — mut result + full scan (`patterns/iter-find-first/c.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let r=mut.-1;
  lp(let i=0;i<xs.len;i=i+1){
    if(r<0&&xs.get(i)>990){r=xs.get(i)}
  };
  <r
};
```

**Sources.**

- `idiom-v0.4#7`
- `card:Loops`

**Lint.** `scan-without-break` — severity `hint`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### iter-count

**Intent.** Count the elements satisfying a predicate.

**Applicability.** No count/any/all combinator exists on 2.8.0 (ABSENT - 131.30 gap: xs.count(&p) would be ~9 tokens). a allocates the filtered array only to read .len; b is the plain loop; c is a reduce with the predicate folded.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | xs.filter(&p).len | 15 | 41 | 15 | 22 | 20 | 41 | 64.50 | 163360 | 225 / 199782840 | 3.36 | slower | best |
| **b** | loop counter | 15 | 88 | 20 | 51 | 49 | 88 | 54.36 | 131328 | 224 / 134246816 | 3.34 | best | best |
| c | xs.reduce(0;&cntev) | 16 | 40 | 16 | 22 | 20 | 40 | 60.32 | 131328 | 224 / 134246816 | 3.66 | slower | tied |

**Verdict.** canonical = `b` (loop counter); status = `provisional`.

**Canonical form** (`patterns/iter-count/b.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isev(x:i64):bool{<x%2==0};
f=cntev(a:i64;x:i64):i64{<if(x%2==0){a+1}el{a}};
f=pat(xs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%2==0){c=c+1}
  };
  <c
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

**Form `a`** — xs.filter(&p).len (`patterns/iter-count/a.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  <xs.filter(&isev).len
};
```

**Form `c`** — xs.reduce(0;&cntev) (`patterns/iter-count/c.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  <xs.reduce(0;&cntev)
};
```

**Sources.**

- `idiom-v0.4#7`
- `card:Arrays and maps`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### iter-nested-early-exit

**Intent.** Find the first (i,j) pair in a nested loop and stop.

**Applicability.** Nested search inside a function: return directly with < from the inner loop (a) instead of a found-flag plus two br (b). Only applies when the search is its own function; inline searches in main still need br.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | nested lp + direct < return | 27 | 124 | 30 | 67 | 63 | 124 | 55.15 | 9776 | 220 / 8417649 | 3.24 | tied | best |
| b | mut flag + br in both loops | 35 | 153 | 37 | 82 | 78 | 153 | 52.85 | 9760 | 220 / 8417649 | 3.39 | best | more |

**Verdict.** canonical = `a` (nested lp + direct < return); status = `provisional`.

**Canonical form** (`patterns/iter-nested-early-exit/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(xs:@i64):i64{
  lp(let i=0;i<xs.len;i=i+1){
    lp(let j=i+1;j<xs.len;j=j+1){
      if(xs.get(i)+xs.get(j)==1985){<i*xs.len+j}
    }
  };
  <-1
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

**Form `b`** — mut flag + br in both loops (`patterns/iter-nested-early-exit/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let r=mut.-1;
  lp(let i=0;i<xs.len;i=i+1){
    lp(let j=i+1;j<xs.len;j=j+1){
      if(xs.get(i)+xs.get(j)==1985){r=i*xs.len+j;br}
    };
    if(r>=0){br}
  };
  <r
};
```

**Sources.**

- `idiom-v0.4#1`
- `card:Loops`

**Lint.** `flag-break-is-return` — severity `hint`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `coll` — Collections

*Intent:* collection queries.

### coll-lookup-default

**Intent.** Read a map value, substituting a default when the key is missing.

**Applicability.** str-keyed maps. m.get(k) on a missing key returns 0 and mt treats the 0 sentinel as $err, so BOTH forms conflate a stored 0 with "missing"; there is no m.has (m.contains silently returns 0 for present keys) - 131.30 gap. The mt arm cannot appear inside a binary expression (t=t+mt ... is E2002); bind it with let first.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | let v=mt m.get(k) {$ok:x x;$err:e d} | 26 | 125 | 35 | 68 | 66 | 125 | 134.28 | 1114288 | 96235 / 1027014082 | 26.11 | best | best |
| b | let v=m.get(k); if(v==0){d}el{v} | 28 | 127 | 35 | 70 | 68 | 127 | 138.37 | 1114384 | 96235 / 1027014082 | 24.57 | tied | more |

**Verdict.** canonical = `a` (let v=mt m.get(k) {$ok:x x;$err:e d}); status = `provisional`.

**Canonical form** (`patterns/coll-lookup-default/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(m:@(str:i64);ks:@str):i64{
  let t=mut.0;
  lp(let i=0;i<ks.len;i=i+1){
    let v=mt m.get(ks.get(i)) {$ok:x x;$err:e 7};
    t=t+v
  };
  <t
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let m=mut.@("k0":1);
  let ks=mut.@();
  lp(let i=1;i<n;i=i+1){m=m.set("k\(i)";i+1)};
  lp(let i=0;i<n;i=i+1){ks=ks+@("k\(i*3)")};
  io.println("t=\(pat(m;ks))");
  <0
};
```

**Form `b`** — let v=m.get(k); if(v==0){d}el{v} (`patterns/coll-lookup-default/b.tk`, `pat` only):

```toke
f=pat(m:@(str:i64);ks:@str):i64{
  let t=mut.0;
  lp(let i=0;i<ks.len;i=i+1){
    let v=m.get(ks.get(i));
    let d=if(v==0){7}el{v};
    t=t+d
  };
  <t
};
```

**Sources.**

- `idiom-v0.4#9`
- `card:Match expression`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### coll-membership

**Intent.** Count how many query values occur in a reference array.

**Applicability.** Repeated membership tests. b (linear scan per query) is O(N*Q) and worse-bigO; a builds a str-keyed map once (int-keyed map literals @(0:1) segfault on .set - 131.30 gap - so keys are "\(x)"). The preferred xs.contains(v) form is blocked by 127.11 (routed to the string glue, SIGSEGV) and would still be O(N*Q).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | map-as-set + mt get | 45 | 205 | 49 | 105 | 102 | 205 | 117.23 | 45568 | 1024288 / 40974484 | 3.46 | best | more |
| b | linear scan with br per query | 24 | 133 | 28 | 73 | 70 | 133 | 20037.72 | 9904 | 239 / 8418066 | 99.00 | worse-bigO | best |
| c | xs.contains(q) per query (blocked 127.11) *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |

**Verdict.** canonical = `a` (map-as-set + mt get); status = `blocked`.

**Canonical form** (`patterns/coll-membership/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(xs:@i64;qs:@i64):i64{
  let set=mut.@("":1);
  lp(let i=0;i<xs.len;i=i+1){set=set.set("\(xs.get(i))";1)};
  let c=mut.0;
  lp(let i=0;i<qs.len;i=i+1){
    let h=mt set.get("\(qs.get(i))") {$ok:v 1;$err:e 0};
    c=c+h
  };
  <c
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  let qs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append(i*3+1);qs=qs.append(i*2+1)};
  io.println("c=\(pat(xs;qs))");
  <0
};
```

**Form `b`** — linear scan with br per query (`patterns/coll-membership/b.tk`, `pat` only):

```toke
f=pat(xs:@i64;qs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<qs.len;i=i+1){
    lp(let j=0;j<xs.len;j=j+1){
      if(xs.get(j)==qs.get(i)){c=c+1;br}
    }
  };
  <c
};
```

**Form `c`** — xs.contains(q) per query (blocked 127.11) — **blocked on 127.11** (`patterns/coll-membership/c.blocked.tk`, not compiled):

```text
f=pat(xs:@i64;qs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<qs.len;i=i+1){
    if(xs.contains(qs.get(i))){c=c+1}
  };
  <c
};
```

**Sources.**

- `card:Arrays and maps`
- `idiom-v0.4#4`

**Bug caveats.**

- [127.11](/docs/progress/): array .contains/.find/.indexof/.slice dispatch to the str glue and segfault; c.blocked.tk is the natural form — preferred when fixed: form `c`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### coll-sort-take

**Intent.** Sort ascending and take the first k elements.

**Applicability.** Top-k. a sorts (comparator &cmp returns a-b) then copies k elements; b does k selection passes (O(N*k), linear for fixed k but 2x the tokens). The natural xs.sort(&cmp).slice(0;k) is blocked by 127.11.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | xs.sort(&cmp) + append first k | 24 | 109 | 24 | 55 | 52 | 109 | 141.90 | 1111712 | 16210 / 1024604997 | 31.30 | best | best |
| b | k selection passes with set-sentinel | 52 | 207 | 59 | 106 | 97 | 207 | 147.37 | 1112864 | 16219 / 1025757213 | 29.06 | tied | more |
| c | xs.sort(&cmp).slice(0;k) (blocked 127.11) *(blocked)* | — | — | — | — | — | — | — | — | — | — | blocked | blocked |

**Verdict.** canonical = `a` (xs.sort(&cmp) + append first k); status = `blocked`.

**Canonical form** (`patterns/coll-sort-take/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=cmp(a:i64;b:i64):i64{<a-b};
f=pat(xs:@i64;k:i64):@i64{
  let so=xs.sort(&cmp);
  let r=mut.@();
  lp(let i=0;i<k;i=i+1){r=r.append(so.get(i))};
  <r
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7919)%10007)};
  let r=pat(xs;10);
  let t=mut.0;
  lp(let i=0;i<r.len;i=i+1){t=t*3+r.get(i)};
  io.println("len=\(r.len) t=\(t)");
  <0
};
```

**Form `b`** — k selection passes with set-sentinel (`patterns/coll-sort-take/b.tk`, `pat` only):

```toke
f=pat(xs:@i64;k:i64):@i64{
  let src=mut.xs;
  let r=mut.@();
  lp(let t=0;t<k;t=t+1){
    let bi=mut.0;
    lp(let i=1;i<src.len;i=i+1){
      if(src.get(i)<src.get(bi)){bi=i}
    };
    r=r.append(src.get(bi));
    src=src.set(bi;1000000000)
  };
  <r
};
```

**Form `c`** — xs.sort(&cmp).slice(0;k) (blocked 127.11) — **blocked on 127.11** (`patterns/coll-sort-take/c.blocked.tk`, not compiled):

```text
f=pat(xs:@i64;k:i64):@i64{
  <xs.sort(&cmp).slice(0;k)
};
```

**Sources.**

- `card:Arrays and maps`

**Bug caveats.**

- [127.11](/docs/progress/): array .slice segfaults; c.blocked.tk (sort().slice(0;k)) is the preferred form — preferred when fixed: form `c`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### coll-group-by

**Intent.** Group values by a derived string key into a map of arrays.

**Applicability.** Grouping. a: map of arrays with mt-get-or-empty then set; b: parallel keys/groups arrays with a linear key scan (O(N*K)). Map literal must be seeded (mut.@("":@())) because an empty map literal crashes on set (131.30 gap); the checksum is computed inside pat because .keys() on a map returned from a user function fails to link.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | map of arrays via mt get-or-@() + set | 64 | 279 | 74 | 136 | 129 | 279 | 55.35 | 358512 | 96238 / 317402549 | 21.08 | best | best |
| b | parallel key array + nested arrays | 89 | 408 | 97 | 181 | 174 | 408 | 55.40 | 358512 | 96249 / 317403133 | 23.08 | tied | more |

**Verdict.** canonical = `a` (map of arrays via mt get-or-@() + set); status = `provisional`.

**Canonical form** (`patterns/coll-group-by/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(xs:@i64):i64{
  let g=mut.@("":@());
  lp(let i=0;i<xs.len;i=i+1){
    let k="g\(xs.get(i)%13)";
    let cur=mt g.get(k) {$ok:v v;$err:e @()};
    g=g.set(k;cur.append(xs.get(i)))
  };
  let ks=g.keys();
  let t=mut.0;
  lp(let i=0;i<ks.len;i=i+1){
    let sz=g.get(ks.get(i)).len;
    t=t+sz*sz
  };
  <t+(ks.len-1)*100000
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

**Form `b`** — parallel key array + nested arrays (`patterns/coll-group-by/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let keys=mut.@();
  let groups=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    let k="g\(xs.get(i)%13)";
    let gi=mut.-1;
    lp(let j=0;j<keys.len;j=j+1){
      if(keys.get(j)==k){gi=j;br}
    };
    if(gi<0){
      keys=keys+@(k);
      groups=groups.append(@(xs.get(i)))
    }el{
      let cur=groups.get(gi);
      groups=groups.set(gi;cur.append(xs.get(i)))
    }
  };
  let t=mut.0;
  lp(let i=0;i<groups.len;i=i+1){
    let sz=groups.get(i).len;
    t=t+sz*sz
  };
  <t+keys.len*100000
};
```

**Sources.**

- `card:Arrays and maps`
- `idiom-v0.4#9`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### coll-reverse

**Intent.** Produce the reversed copy of an array.

**Applicability.** No reverse combinator exists (131.30 gap). a appends from the end; b copies and swaps in place; c prepends with @(x)+r and is quadratic (each + copies the accumulator) - token-tied with a but worse-bigO, so it must never be taught.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | loop from end + append | 16 | 90 | 24 | 47 | 45 | 90 | 148.29 | 1111872 | 16219 / 1024739107 | 23.46 | best | best |
| b | copy + set swap to the middle | 24 | 130 | 34 | 70 | 68 | 130 | 449.68 | 3119888 | 32203 / 3072860587 | 47.07 | worse-bigO | more |
| c | prepend with @(x)+r | 16 | 82 | 19 | 46 | 44 | 82 | 280.16 | 2222224 | 48204 / 2049436611 | 30.57 | slower | best |

**Verdict.** canonical = `a` (loop from end + append); status = `provisional`.

**Canonical form** (`patterns/coll-reverse/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(xs:@i64):@i64{
  let r=mut.@();
  lp(let i=xs.len-1;i>=0;i=i-1){r=r.append(xs.get(i))};
  <r
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  let r=pat(xs);
  let t=mut.0;
  lp(let i=0;i<r.len;i=i+1){t=(t*31+r.get(i))%1000000007};
  io.println("len=\(r.len) t=\(t)");
  <0
};
```

**Form `b`** — copy + set swap to the middle (`patterns/coll-reverse/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let r=mut.xs;
  let n=xs.len;
  lp(let i=0;i<n/2;i=i+1){
    let t=r.get(i);
    r=r.set(i;r.get(n-1-i));
    r=r.set(n-1-i;t)
  };
  <r
};
```

**Form `c`** — prepend with @(x)+r (`patterns/coll-reverse/c.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let r=mut.@();
  lp(let i=0;i<xs.len;i=i+1){r=@(xs.get(i))+r};
  <r
};
```

**Sources.**

- `card:Arrays and maps`
- `ast-mine:1`

**Lint.** `quadratic-prepend` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### coll-swap

**Intent.** Swap two elements of an array.

**Applicability.** Any element swap (sort inner loops). Chained r=r.set(i;r.get(j)).set(j;r.get(i)) is correct under value semantics (the second r.get reads the original) and one token cheaper than the tmp form the card shows; verified byte-identical output.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| a | let tmp + two set statements (card form) | 26 | 126 | 33 | 68 | 66 | 126 | 109.46 | 807072 | 16203 / 768444572 | 23.66 | slower | tied |
| **b** | chained .set().set() | 25 | 114 | 30 | 63 | 61 | 114 | 102.11 | 807072 | 16203 / 768444572 | 24.32 | best | best |

**Verdict.** canonical = `b` (chained .set().set()); status = `provisional`.

**Canonical form** (`patterns/coll-swap/b.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(xs:@i64):@i64{
  let r=mut.xs;
  let n=xs.len;
  lp(let i=0;i<n-1;i=i+2){
    r=r.set(i;r.get(i+1)).set(i+1;r.get(i))
  };
  <r
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  let r=pat(xs);
  let t=mut.0;
  lp(let i=0;i<r.len;i=i+1){t=(t*31+r.get(i))%1000000007};
  io.println("len=\(r.len) t=\(t)");
  <0
};
```

**Form `a`** — let tmp + two set statements (card form) (`patterns/coll-swap/a.tk`, `pat` only):

```toke
f=pat(xs:@i64):@i64{
  let r=mut.xs;
  let n=xs.len;
  lp(let i=0;i<n-1;i=i+2){
    let t=r.get(i);
    r=r.set(i;r.get(i+1));
    r=r.set(i+1;t)
  };
  <r
};
```

**Sources.**

- `card:Arrays and maps`
- `ast-mine:1`

**Lint.** `swap-tmp-let` — severity `hint`, auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### coll-dedupe

**Intent.** Count (or collect) the distinct values of an array.

**Applicability.** Dedupe. b (nested scan over the unique list) is token-best but O(N*U) - worse-bigO when uniques grow with N - so the map-as-set form is canonical without a hot path. Int-keyed maps segfault (131.30 gap), so the set key is "\(x)".

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | str-keyed map-as-set + mt get | 41 | 187 | 50 | 94 | 92 | 187 | 58.82 | 37632 | 896268 / 34623588 | 4.57 | best | more |
| b | nested scan over unique list | 34 | 182 | 36 | 84 | 82 | 182 | 6719.10 | 9392 | 239 / 8418051 | 17.11 | worse-bigO | best |

**Verdict.** canonical = `a` (str-keyed map-as-set + mt get); status = `provisional`.

**Canonical form** (`patterns/coll-dedupe/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(xs:@i64):i64{
  let seen=mut.@("":1);
  let c=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    let x=xs.get(i);
    let dup=mt seen.get("\(x)") {$ok:v 1;$err:e 0};
    if(dup==0){seen=seen.set("\(x)";1);c=c+1}
  };
  <c
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%(n*3/4))};
  io.println("u=\(pat(xs))");
  <0
};
```

**Form `b`** — nested scan over unique list (`patterns/coll-dedupe/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let u=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    let x=xs.get(i);
    let dup=mut.false;
    lp(let j=0;j<u.len;j=j+1){
      if(u.get(j)==x){dup=true;br}
    };
    if(!dup){u=u.append(x)}
  };
  <u.len
};
```

**Sources.**

- `card:Arrays and maps`

**Lint.** `quadratic-dedupe` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `cli` — Program boundary

*Intent:* argv, stdin and printing results.

### cli-argv

**Intent.** Read the first program argument with a default when absent.

**Applicability.** args.get(n) returns $err past the end, so mt is a one-expression form; the count check is 7 tokens more. The args.get result interpolates as a pointer (127.7 class) - print it directly or take s.len. args.count() includes the program name.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | mt args.get(1) {$ok:v v;$err:e "default"} | 8 | 47 | 14 | 23 | 23 | 53 | 62.19 | 1424 | 199 / 28490 | 3.65 | best | best |
| b | if(args.count()>1){args.get(1)}el{"default"} | 15 | 52 | 18 | 22 | 22 | 58 | 64.03 | 1424 | 199 / 28490 | 3.58 | tied | more |

**Verdict.** canonical = `a` (mt args.get(1) {$ok:v v;$err:e "default"}); status = `provisional`.

**Canonical form** (`patterns/cli-argv/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=args:std.args;
f=pat():str{
  <mt args.get(1) {$ok:v v;$err:e "default"}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+s.len(pat())};
  io.println("t=\(t)");
  <0
};
```

**Form `b`** — if(args.count()>1){args.get(1)}el{"default"} (`patterns/cli-argv/b.tk`, `pat` only):

```toke
f=pat():str{
  <if(args.count()>1){args.get(1)}el{"default"}
};
```

**Sources.**

- `idiom-v0.4#9`
- `card:Std modules`

**Bug caveats.**

- [127.7](/docs/progress/): "\(args.get(1))" prints a pointer

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### cli-flag-filter

**Intent.** Drop --flag arguments from an argv array.

**Applicability.** The D-CLI "nonflags" shape: filter with a named predicate vs loop+if+append. The loop form builds with r=r+@(x) because an .append-built @str interpolates as pointers until 127.10 lands; the filter result is read directly in the fixture.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | av.filter(&notflag) | 14 | 41 | 17 | 17 | 17 | 41 | 132.06 | 1113024 | 64204 / 1025649505 | 26.45 | best | best |
| b | loop + if + r=r+@(x) | 18 | 116 | 25 | 53 | 53 | 117 | 201.85 | 1622704 | 85536 / 1481215689 | 40.40 | worse-bigO | more |

**Verdict.** canonical = `a` (av.filter(&notflag)); status = `provisional`.

**Canonical form** (`patterns/cli-flag-filter/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=notflag(x:str):bool{<!s.startswith(x;"--")};
f=pat(av:@str):@str{
  <av.filter(&notflag)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let av=mut.@();
  lp(let i=0;i<n;i=i+1){av=av+@(if(i%3==0){"--flag\(i)"}el{"arg\(i)"})};
  let r=pat(av);
  let t=mut.0;
  lp(let i=0;i<r.len;i=i+1){t=t+s.len(r.get(i))};
  io.println("n=\(r.len) t=\(t)");
  <0
};
```

**Form `b`** — loop + if + r=r+@(x) (`patterns/cli-flag-filter/b.tk`, `pat` only):

```toke
f=pat(av:@str):@str{
  let r=mut.@();
  lp(let i=0;i<av.len;i=i+1){
    if(!s.startswith(av.get(i);"--")){r=r+@(av.get(i))}
  };
  <r
};
```

**Sources.**

- `ast-mine:4`
- `idiom-v0.4#7`

**Bug caveats.**

- [127.10](/docs/progress/): .append-built string arrays print addresses when an element is interpolated; the loop form uses +@() instead

**Lint.** `loop-is-filter` — severity `hint`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### cli-print-results

**Intent.** Format a labelled result as "key=value".

**Applicability.** The 8.5% io.println(show(x)) shape (131.3). For i64/str values interpolation is the best form today; there is no show for bool (prints 1/0 - 127.15) or for arrays/maps (131.30 stdlib gap - a stdlib show/fmt would replace the hand formatters). Values derived from method calls interpolate as pointers (127.7).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | "\(k)=\(v)" interpolation | 10 | 36 | 17 | 22 | 21 | 36 | 97.10 | 65776 | 3072199 / 35271051 | 3.83 | slower | best |
| b (hot path) | nested s.concat + s.fromint | 15 | 63 | 21 | 28 | 27 | 63 | 84.46 | 57792 | 2560199 / 33735051 | 3.69 | best | more |
| c | s.builder add/add/add | 16 | 99 | 34 | 42 | 41 | 99 | 93.48 | 73840 | 2560199 / 71196524 | 3.76 | slower | more |

**Verdict.** canonical = `a` ("\(k)=\(v)" interpolation); hot path = `b` (nested s.concat + s.fromint) — choose it when the line is formatted inside a loop executed millions of times: `s.concat` + `s.fromint` measured 84.5 ms against 97.1 ms for interpolation (+15%) with 0.5M fewer allocation calls, for 5 tokens more; status = `provisional`.

**Canonical form** (`patterns/cli-print-results/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(k:str;v:i64):str{
  <"\(k)=\(v)"
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+s.len(pat("r\(i%5)";i*13))};
  io.println("t=\(t)");
  <0
};
```

**Form `b`** (hot path) — nested s.concat + s.fromint (`patterns/cli-print-results/b.tk`, `pat` only):

```toke
f=pat(k:str;v:i64):str{
  <s.concat(s.concat(k;"=");s.fromint(v))
};
```

**Form `c`** — s.builder add/add/add (`patterns/cli-print-results/c.tk`, `pat` only):

```toke
f=pat(k:str;v:i64):str{
  let b=s.builder();
  s.add(b;k);
  s.add(b;"=");
  s.add(b;s.fromint(v));
  <s.build(b)
};
```

**Sources.**

- `idiom-v0.4#5`
- `ast-mine:4`
- `card:Strings`

**Bug caveats.**

- [127.15](/docs/progress/): bools interpolate as 1/0, so a bool result still needs a hand formatter
- [127.7](/docs/progress/): method-call-derived str values print a pointer under interpolation

**Lint.** `nested-concat` — severity `warning`, auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `fn` — Decomposition

*Intent:* helpers, chaining and recursion.

### fn-helper-vs-inline

**Intent.** Test a compound condition inside a loop: named helper or inline expression.

**Applicability.** Protocol region counts only pat: the helper call (16) beats the inline condition (21) but the helper itself costs 14 proxy8k tokens (isok: min_bytes 35), so whole-program the inline form wins for a single call site; a helper pays for itself from the second call site. -O2 is expected to inline the call.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | call named helper isok(x) | 16 | 89 | 20 | 50 | 48 | 89 | 64.97 | 131328 | 224 / 134246852 | 3.42 | tied | best |
| b | inline x%3==0&&x%5!=0 | 21 | 104 | 27 | 59 | 57 | 104 | 64.88 | 131344 | 224 / 134246852 | 3.34 | best | more |

**Verdict.** canonical = `a` (call named helper isok(x)); status = `provisional`.

**Canonical form** (`patterns/fn-helper-vs-inline/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isok(x:i64):bool{<x%3==0&&x%5!=0};
f=pat(xs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    if(isok(xs.get(i))){c=c+1}
  };
  <c
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

**Form `b`** — inline x%3==0&&x%5!=0 (`patterns/fn-helper-vs-inline/b.tk`, `pat` only):

```toke
f=pat(xs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%3==0&&xs.get(i)%5!=0){c=c+1}
  };
  <c
};
```

**Sources.**

- `idiom-v0.4#8`
- `card:Program conventions and idiom`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### fn-chain-vs-let

**Intent.** Read one field of a split line.

**Applicability.** Single-use intermediates. Chained postfix (a) and the two-let form (b) count the same proxy8k tokens (11) - the hostile proxy has cheap merges for let - but a is 34 bytes shorter (min_bytes tie-break) and 3 tokens shorter under v03.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | s.len(s.split(line;",").get(1)) | 11 | 53 | 15 | 24 | 23 | 53 | 66.46 | 49792 | 2048199 / 34255186 | 3.78 | best | best |
| b | let parts=...; let second=parts.get(1); s.len(second) | 11 | 87 | 18 | 31 | 30 | 87 | 74.28 | 49808 | 2048199 / 34255186 | 3.13 | slower | best |

**Verdict.** canonical = `a` (s.len(s.split(line;",").get(1))); status = `provisional`.

**Canonical form** (`patterns/fn-chain-vs-let/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(line:str):i64{
  <s.len(s.split(line;",").get(1))
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+pat("ab,c\(i),de")};
  io.println("t=\(t)");
  <0
};
```

**Form `b`** — let parts=...; let second=parts.get(1); s.len(second) (`patterns/fn-chain-vs-let/b.tk`, `pat` only):

```toke
f=pat(line:str):i64{
  let parts=s.split(line;",");
  let second=parts.get(1);
  <s.len(second)
};
```

**Sources.**

- `idiom-v0.4#6`
- `idiom-v0.4#8`
- `card:Program conventions and idiom`

**Lint.** `single-use-let` — severity `hint`, auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### fn-recursion-vs-loop

**Intent.** Sum the decimal digits of an integer.

**Applicability.** Depth-bounded recursion (<= 19 frames). Recursion is 5 tokens cheaper; the loop avoids call overhead. Never recurse on unbounded depth (no TCO; stack).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | expression-if recursion | 15 | 48 | 19 | 33 | 28 | 48 | 112.60 | 1440 | 199 / 28538 | 3.55 | slower | best |
| b (hot path) | lp with mut accumulator | 20 | 83 | 26 | 55 | 51 | 83 | 77.19 | 1424 | 199 / 28538 | 4.08 | best | more |

**Verdict.** canonical = `a` (expression-if recursion); hot path = `b` (lp with mut accumulator) — choose it when the call sits in a loop executed > 1M times, or the recursion depth is data-dependent (stack safety): the `lp` accumulator measured 77.2 ms against 112.6 ms (-31%), for 5 tokens more; status = `provisional`.

**Canonical form** (`patterns/fn-recursion-vs-loop/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(x:i64):i64{
  <if(x<10){x}el{x%10+pat(x/10)}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+pat(i*7919+123456789)};
  io.println("t=\(t)");
  <0
};
```

**Form `b`** (hot path) — lp with mut accumulator (`patterns/fn-recursion-vs-loop/b.tk`, `pat` only):

```toke
f=pat(x:i64):i64{
  let t=mut.0;
  let v=mut.x;
  lp(let k=0;v>0;k=k+1){t=t+v%10;v=v/10};
  <t
};
```

**Sources.**

- `idiom-v0.4#1`
- `card:if is an EXPRESSION`

**Lint.** none — the non-canonical forms are not AST-decidable with low false positives.

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

## Family `io` — Files

*Intent:* reading and writing files.

### io-read-lines

**Intent.** Read a file and split it into lines.

**Applicability.** Whole-file reads. No file.readlines exists (131.30 gap); s.split(txt;"\n") yields a trailing "" when the file ends in a newline - both forms keep it. file.read needs --allow-read (fixtures build with --allow-all).

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | mt file.read + s.split "\n" | 19 | 86 | 24 | 35 | 35 | 87 | 68.14 | 47152 | 1024227 / 55179103 | 3.28 | best | best |
| b | mt file.read + charcode scan + slice per line | 45 | 228 | 63 | 95 | 94 | 228 | 30000.00 | pending | pending | 99.00 | worse-bigO | more |

**Verdict.** canonical = `a` (mt file.read + s.split "\n"); status = `provisional`.

**Canonical form** (`patterns/io-read-lines/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;
f=pat(path:str):@str{
  let txt=mt file.read(path) {$ok:v v;$err:e <@()};
  <s.split(txt;"\n")
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let path="/tmp/pat-io-read-lines-\(n).txt";
  let b=s.builder();
  lp(let i=0;i<n;i=i+1){s.add(b;"line \(i) of the file\n")};
  let w=mt file.write(path;s.build(b)) {$ok:v 1;$err:e <1};
  let ls=pat(path);
  let t=mut.ls.len*100;
  lp(let i=0;i<ls.len;i=i+1){t=t+s.len(ls.get(i))};
  io.println("t=\(t)");
  <0
};
```

**Form `b`** — mt file.read + charcode scan + slice per line (`patterns/io-read-lines/b.tk`, `pat` only):

```toke
f=pat(path:str):@str{
  let txt=mt file.read(path) {$ok:v v;$err:e <@()};
  let r=mut.@();
  let st=mut.0;
  let n=s.len(txt);
  lp(let i=0;i<n;i=i+1){
    if(s.charcode(txt;i)==10){r=r.append(s.slice(txt;st;i));st=i+1}
  };
  <r.append(s.slice(txt;st;n))
};
```

**Sources.**

- `idiom-v0.4#4`
- `card:Std modules`

**Lint.** `hand-rolled-parser` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).

### io-write-accumulate

**Intent.** Write N lines to a file.

**Applicability.** Append-per-line is token-best (23 vs 27) but reopens the file on every call (a syscall per line, same asymptotic class); the protocol therefore makes it canonical with a hot path - flagged for owner review: a 10-100x constant-factor loss with identical bigO is exactly the case rule 6.4 does not catch. Fixture deletes the file first because file.append accumulates across runs.

| form | label | proxy8k | byte256 | v03 | qwen | cl100k | min bytes | wall ms | RSS KB | allocs calls / bytes | bigO ratio | runtime | tokens |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| **a** | s.builder then one file.write | 27 | 135 | 33 | 61 | 59 | 139 | 59.18 | 31200 | 1024232 / 30587474 | 3.39 | best | more |
| b | file.append per line | 23 | 107 | 25 | 53 | 51 | 111 | 30000.00 | pending | pending | 99.00 | worse-bigO | best |

**Verdict.** canonical = `a` (s.builder then one file.write); status = `provisional`.

**Canonical form** (`patterns/io-write-accumulate/a.tk`, full fixture program):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;
f=pat(path:str;n:i64):i64{
  let b=s.builder();
  lp(let i=0;i<n;i=i+1){s.add(b;"row \(i)\n")};
  <mt file.write(path;s.build(b)) {$ok:v 1;$err:e 0}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let path="/tmp/pat-io-write-acc-\(n).txt";
  if(file.exists(path)){let d=mt file.delete(path) {$ok:v 1;$err:e <1}};
  let ok=pat(path;n);
  let txt=mt file.read(path) {$ok:v v;$err:e <2};
  io.println("ok=\(ok) len=\(s.len(txt)) lines=\(s.split(txt;"\n").len)");
  <0
};
```

**Form `b`** — file.append per line (`patterns/io-write-accumulate/b.tk`, `pat` only):

```toke
f=pat(path:str;n:i64):i64{
  lp(let i=0;i<n;i=i+1){
    let ok=mt file.append(path;"row \(i)\n") {$ok:v 1;$err:e <0}
  };
  <1
};
```

**Sources.**

- `card:Std modules`
- `idiom-v0.4#5`

**Lint.** `append-in-loop` — severity `warning`, not auto-fixable (`tkc --lint`, story 131.9).

**Measured at.** tkc `toke 2.8.0` @ `5811622433be`; proxy `3c6fbf1909bb`; corpus `5f3f74314ce4`; bench result `20260919-030507.json`; date 2026-09-18. Timed on a machine under load (`meta.load_warning`) — every verdict here stays `provisional` until story 131.25 re-measures (protocol §8).
