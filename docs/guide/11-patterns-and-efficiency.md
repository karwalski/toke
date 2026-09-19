---
title: Lesson 11 — Patterns and Efficiency
slug: 11-patterns-and-efficiency
section: learn
order: 11
---

> **GENERATED** by `scripts/patterns/render_catalogue.py guide` from `patterns/catalogue.json` (sha256 `6dad9e907cfb`). Do not hand-edit: change the catalogue, run `make render-patterns`; `make check-patterns` fails CI on drift.

**Estimated time: ~40 minutes**

toke has one right way to write each everyday construct, and that way is chosen by measurement rather than taste: the form that costs the fewest tokens under the decision tokenizer *and* runs as fast, in as little memory, as any alternative. This lesson walks through the measured pattern catalogue family by family. For every pattern you see the form to write, the form to stop writing, and the numbers that decided it. Where the token-cheapest form is not the fastest, the catalogue names a *hot path* form and the rule for when to reach for it.

Each example is a complete program exactly as the benchmark harness runs it: `pat` is the pattern under measurement, `main` builds an input sized by `PAT_N` and prints one checksum line. Copy the shape of `pat`; the harness around it is the same for every form. A `pending` cell is a runtime number not yet ingested from the bench, and every verdict stays provisional until it is re-measured on the rewritten corpus — see the [normative catalogue](/docs/spec/patterns-v0.4/) for all columns and the [protocol](/docs/spec/patterns-protocol-v0.4/) for how verdicts are decided.

## Conditionals (`cond`)

This family covers conditional binding and boolean logic.

### cond-bind-if

Bind a value that depends on a condition and use it afterwards.

Two-way choice whose result feeds a later expression. Form c (return in each branch) only applies when the value is returned immediately and the continuation is tiny (here `*x+g`, 5 bytes); it duplicates the continuation, so with any reused or longer continuation the expression-if bind (a) wins. Form b is the mut-flag anti-pattern (idiom rule 1).

**Write this** — statement-if returning from each branch (`patterns/cond-bind-if/c.tk`):

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

**Not this** — expression-if bound with let (`patterns/cond-bind-if/a.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64):i64{
  let g=if(x>0){1}el{2};
  <g*x+g
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 17 | 46 | 66.92 | 1440 |
| `b` — avoid | 20 | 56 | 63.24 | 1440 |
| `c` — canonical | 15 | 43 | 62.39 | 1424 |

`tkc --lint` reports the non-canonical forms as `mut-flag-if`.

### cond-elif-chain

Choose one of several values from an ordered set of threshold tests.

Three or more mutually exclusive conditions tested in order (grades, buckets, tiers). Form c (a ladder of independent ifs overwriting one mut) is only equivalent when later tests subsume earlier ones, so it is fragile as well as verbose.

**Write this** — `el if` chain as one expression (`patterns/cond-elif-chain/a.tk`):

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

**Not this** — mut-flag ladder of independent ifs (`patterns/cond-elif-chain/c.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64):i64{
  let g=mut.1;
  if(x>70){g=2};
  if(x>80){g=3};
  if(x>90){g=4};
  <g
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 21 | 63 | 63.55 | 1424 |
| `b` — avoid | 22 | 65 | 65.13 | 1440 |
| `c` — avoid | 28 | 74 | 65.99 | 1424 |

`tkc --lint` reports the non-canonical forms as `mut-flag-if`.

### cond-bool-combine

Compute a value from the OR of two tests.

Any boolean combination; `&&`/`||` short-circuit. Form b (flag soup) evaluates every test; form c (sequential early returns) works only when the result is returned directly.

**Write this** — `||` inside one expression-if (`patterns/cond-bool-combine/a.tk`):

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

**Not this** — flag soup: two ifs setting one mut (`patterns/cond-bool-combine/b.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(a:i64;b:i64):i64{
  let ok=mut.0;
  if(a>0){ok=1};
  if(b>0){ok=1};
  <ok
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 10 | 45 | 94.24 | 1440 |
| `b` — avoid | 17 | 68 | 91.24 | 1440 |
| `c` — avoid | 13 | 50 | 90.25 | 1440 |

`tkc --lint` reports the non-canonical forms as `flag-soup`.

### cond-clamp

Clamp a value into [lo,hi] (min/max).

Any bounded value; the stdlib has no min/max combinators (ABSENT per combinator-status), so an expression-if chain is the primitive. A helper-function form was not measured because helper bodies fall outside the counted `pat` region.

**Write this** — `el if` chain expression (`patterns/cond-clamp/a.tk`):

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

**Not this** — mut copy then two clamping ifs (`patterns/cond-clamp/b.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64;lo:i64;hi:i64):i64{
  let r=mut.x;
  if(r<lo){r=lo};
  if(r>hi){r=hi};
  <r
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 11 | 65 | 54.50 | 1424 |
| `b` — avoid | 18 | 76 | 68.43 | 1440 |
| `c` — avoid | 17 | 62 | 54.67 | 1440 |

`tkc --lint` reports the non-canonical forms as `mut-flag-if`.

### cond-bool-render

Render a boolean test as the text `true`/`false`.

Printing or storing a bool as text. Interpolating a bool prints `1`/`0` on tkc 2.8.0 (127.15), so form b changes the output and is blocked; 8.5% of corpus programs hand-write a boolstr helper for this (131.30).

The preferred way to write this — interpolate the bool `"\(c)"` — is blocked on compiler story 127.15, so today's canonical form is the best form that works. The blocked form, for reference (not compiled):

```text
f=pat(x:i64):str{
  <"\(x%2==0)"
};
```

**Write this** — expression-if selecting the literal (`patterns/cond-bool-render/a.tk`):

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

**Not this** — mut string flag overwritten by an if (`patterns/cond-bool-render/c.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(x:i64):str{
  let r=mut."false";
  if(x%2==0){r="true"};
  <r
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 8 | 48 | 56.93 | 1408 |
| `c` — avoid | 13 | 59 | 55.06 | 1424 |

`tkc --lint` reports the non-canonical forms as `mut-flag-if`.

## Accumulation (`acc`)

This family covers accumulation into a value or collection.

### acc-sum

Sum the elements of an @i64.

Numeric reduction over an array. The reduce form needs a two-arg helper function (`addf`) declared outside `pat`; its tokens are not counted in the measured region, so the token tie flatters b. `fold` is ABSENT (127.4); `reduce` is the canonical combinator.

**Write this** — lp accumulating into a mut (`patterns/acc-sum/a.tk`):

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

**Not this** — `xs.reduce(0;&addf)` (`patterns/acc-sum/b.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=addf(a:i64;b:i64):i64{<a+b};
f=pat(xs:@i64):i64{
  <xs.reduce(0;&addf)
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 16 | 76 | 63.20 | 232896 |
| `b` — avoid | 16 | 39 | 69.71 | 232896 |

### acc-count-if

Count the elements satisfying a predicate.

Count with a per-element test. Forms b and c need a helper predicate/step function outside `pat` (not counted). b materialises the filtered array just to take its length.

**Write this** — lp with if and counter (`patterns/acc-count-if/a.tk`):

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

**Not this** — `xs.reduce(0;&step)` with expr-if step (`patterns/acc-count-if/c.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=cnt3(a:i64;x:i64):i64{<if(x%3==0){a+1}el{a}};
f=pat(xs:@i64):i64{
  <xs.reduce(0;&cnt3)
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 16 | 88 | 73.58 | 232896 |
| `b` — avoid | 15 | 41 | 89.16 | 266192 |
| `c` — avoid | 16 | 39 | 86.99 | 232896 |

### acc-min-max

Find the minimum (symmetrically maximum) of a non-empty @i64.

Non-empty arrays only (all forms index element 0). `min`/`max` combinators are ABSENT. Form c sorts a copy (qsort, O(N log N), one N-word allocation) to read element 0; its 4N/N ratio stays within 1.5x of linear so the protocol classes it `slower`, not `worse-bigO` — hence the hot_path rule.

**Write this** — `xs.sort(&cmp).get(0)` (`patterns/acc-min-max/c.tk`):

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

**Not this** — `xs.reduce(xs.get(0);&mn)` (`patterns/acc-min-max/b.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=mn(a:i64;b:i64):i64{<if(b<a){b}el{a}};
f=pat(xs:@i64):i64{
  <xs.reduce(xs.get(0);&mn)
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

**Hot path** — lp tracking the running minimum (`patterns/acc-min-max/a.tk`). Choose it when arrays larger than ~1k elements, or the minimum is taken inside a loop body: sort is O(N log N) plus a full copy, the loop is O(N) with no allocation.

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(xs:@i64):i64{
  let m=mut.xs.get(0);
  lp(let i=1;i<xs.len;i=i+1){
    if(xs.get(i)<m){m=xs.get(i)}
  };
  <m
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — hot path | 18 | 99 | 60.29 | 232896 |
| `b` — avoid | 18 | 45 | 69.16 | 232896 |
| `c` — canonical | 14 | 41 | 583.35 | 332928 |

### acc-array

Accumulate values into a new @i64 in a loop.

Any loop that collects results. `x=x.append(v)` at a self-update site is lowered to the in-place amortised append (ADR-0006 D2); `x=x+@(v)` calls tk_array_concat, which mallocs and copies the whole array every iteration (O(N^2) bytes, never freed). std.vec is a separate handle type needing `vec.toarray` at the end.

**Write this** — `x=x.append(v)` (`patterns/acc-array/a.tk`):

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

**Not this** — `x=x+@(v)` (`patterns/acc-array/b.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(n:i64):@i64{
  let x=mut.@();
  lp(let i=0;i<n;i=i+1){
    x=x+@(i*3)
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 13 | 74 | 81.94 | 260912 |
| `b` — avoid | 13 | 69 | 30000.00 | pending |
| `c` — avoid | 27 | 95 | 86.59 | 259664 |

### acc-map-build

Accumulate per-key totals for a small fixed key set.

Keys drawn from a known set of ~4 strings. Both forms are a linear key scan (the 2.8.0 map is an unsorted entry list searched with strcmp; `m.set` updates in place). Form b only applies when the key set is fixed and small; with an open key set the map is the only correct form. Int-keyed maps crash on 2.8.0, so keys must be str.

**Write this** — parallel arrays + linear key search + `c=c.set(j;…)` (`patterns/acc-map-build/b.tk`):

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

**Not this** — seeded map, `m=m.set(k;m.get(k)+v)` (`patterns/acc-map-build/a.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(keys:@str;n:i64):i64{
  let m=mut.@("a":0;"b":0;"c":0;"d":0);
  lp(let i=0;i<n;i=i+1){
    let k=keys.get(i%4);
    m=m.set(k;m.get(k)+i)
  };
  <m.get("a")+2*m.get("b")+3*m.get("c")+4*m.get("d")
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let keys=@("a";"b";"c";"d");
  io.println("chk=\(pat(keys;n))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 50 | 180 | 77.18 | 1456 |
| `b` — canonical | 48 | 209 | 56.64 | 1456 |

### acc-dedupe

Remove duplicate values from an @i64 (order not significant).

Dedupe where result order is free (checksum is len+sum). Form a (array `.contains`) is the natural form and, since 127.11 closed (2026-09-19), it compiles, runs and matches its siblings — it is token-best (16 vs 25/31) and the same quadratic class as every other form here, 11.5% behind the seen-map. Form d keys a map by the interpolated value: one interpolation alloc per element (48.2k calls vs 16.2k) and still quadratic.

**Write this** — `if(!out.contains(v)){out=out.append(v)}` (`patterns/acc-dedupe/a.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(xs:@i64):@i64{
  let out=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    let v=xs.get(i);
    if(!out.contains(v)){out=out.append(v)}
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

**Not this** — sort a copy, append when != previous (`patterns/acc-dedupe/c.tk`):

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

**Hot path** — seen-map keyed by `"\(v)"` (`patterns/acc-dedupe/d.tk`). Choose it when the seen-set scan dominates and a constant factor is worth 15 tokens: the seen-map (d) measured 120.7 ms against 134.6 ms for `out.contains(v)` (+11.5%) at N=16000, but allocates 3x as much (48.2k calls vs 16.2k) and is just as quadratic (bigO 26.8 vs 26.0) — neither form fixes the class, so prefer `a` unless the profile shows the membership scan on top..

```toke
m=main;
i=io:std.io;
i=env:std.env;
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 16 | 125 | 134.62 | 1111648 |
| `b` — avoid | 25 | 186 | 129.17 | 1111648 |
| `c` — avoid | 31 | 150 | 127.72 | 1111808 |
| `d` — hot path | 31 | 176 | 120.71 | 1112672 |

## Strings (`str`)

This family covers building and formatting strings.

### str-build-loop

Build one string from N parts in a loop.

Any loop that appends text. `s.concat` allocates a fresh copy of the whole accumulator each step (O(N^2) bytes, never freed); the builder grows one buffer; append+join appends in place and allocates once at join.

**Write this** — `s.builder` / `s.add` / `s.build` (`patterns/str-build-loop/b.tk`):

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

**Not this** — `r=s.concat(r;part)` chain (`patterns/str-build-loop/a.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(parts:@str;n:i64):str{
  let r=mut."";
  lp(let i=0;i<n;i=i+1){
    r=s.concat(r;parts.get(i%4))
  };
  <r
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let parts=@("ab";"cde";"f";"ghij");
  let r=pat(parts;n);
  io.println("len=\(s.len(r)) f=\(s.split(r;"f").len)");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 15 | 96 | 30000.00 | pending |
| `b` — canonical | 13 | 105 | 58.83 | 44832 |
| `c` — avoid | 15 | 114 | 86.66 | 108720 |

`tkc --lint` reports the non-canonical forms as `string-concat-chain`.

### str-interp-vs-join

Assemble a 3-5 part string from values of mixed type.

Templating a fixed number of parts. Interpolation lowers to one tk_str_join_n call (1 alloc); `s.join` needs an array literal plus `s.fromint` for the number (3 allocs); nested `s.concat` allocates per step (5 allocs).

**Write this** — interpolation `"\(a)-\(b)-\(c)"` (`patterns/str-interp-vs-join/a.tk`):

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

**Not this** — nested `s.concat` chain (`patterns/str-interp-vs-join/c.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(a:str;b:str;c:i64):str{
  <s.concat(s.concat(s.concat(s.concat(a;"-");b);"-");s.fromint(c))
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 13 | 47 | 82.40 | 39136 |
| `b` — avoid | 15 | 62 | 85.03 | 76864 |
| `c` — avoid | 20 | 95 | 104.09 | 76784 |

`tkc --lint` reports the non-canonical forms as `string-concat-chain`.

### str-num-format

Convert an i64 to its decimal string.

Number to text with no width/precision. All three forms reach tk_str_fromi64 (1 alloc). `n as str` is accepted by 2.8.0 although the card lists only numeric casts.

**Write this** — `n as str` (`patterns/str-num-format/c.tk`):

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

**Not this** — interpolation `"\(n)"` (`patterns/str-num-format/a.tk`):

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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 7 | 25 | 77.50 | 48544 |
| `b` — avoid | 8 | 31 | 55.62 | 32832 |
| `c` — canonical | 8 | 27 | 55.78 | 32816 |

### str-repeat-pad

Left-pad a number's text to a fixed width with spaces.

Fixed-width formatting (tables, ids). Widths are small constants, so every form is a handful of allocations; `s.repeat(str;u64)` exists in 2.8.0 and interpolates correctly. Forms c/d guard `p>0` because `s.repeat` takes a u64.

**Write this** — lp prepending with `s.concat` (`patterns/str-repeat-pad/a.tk`):

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

**Not this** — interpolate `"\(s.repeat(" ";p))\(d)"` (`patterns/str-repeat-pad/d.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(n:i64;w:i64):str{
  let d=s.fromint(n);
  let p=w-s.len(d);
  <if(p>0){"\(s.repeat(" ";p))\(d)"}el{d}
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

**Hot path** — `s.concat(s.repeat(" ";p);d)` (`patterns/str-repeat-pad/c.tk`). Choose it when every row of a large table is padded: `s.concat(s.repeat(" ";p);d)` measured 67.8 ms against 78.6 ms for the prepend loop (+16%) and 26.6 MB peak RSS against 32.8 MB, for 3 tokens more.

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(n:i64;w:i64):str{
  let d=s.fromint(n);
  let p=w-s.len(d);
  <if(p>0){s.concat(s.repeat(" ";p);d)}el{d}
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 25 | 97 | 100.69 | 65616 |
| `b` — avoid | 25 | 126 | 95.56 | 76784 |
| `c` — hot path | 28 | 102 | 85.29 | 51696 |
| `d` — avoid | 28 | 99 | 96.77 | 51744 |

### str-array-render

Collect N labels into a @str, then render each one.

The collect-then-format shape of CLI/report tasks. Form a (interpolating an element of a `.append`-built `mut.@()`) printed an address until 127.10 closed; since 2026-09-19 it runs and matches its siblings, and it ties c on tokens (23) but is 10.2% slower and allocates 33% more (2.05M vs 1.54M calls). Form b uses the only append form that tags the array (`+@()`) but copies the array every iteration and is quadratic (timeout at 4N). Form c keeps `.append` and reads elements directly into the builder.

**Write this** — `.append`-built, direct `s.add(b;labels.get(i))` reads (`patterns/str-array-render/c.tk`):

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

**Not this** — `+@()`-built, interpolated read (`patterns/str-array-render/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let r=pat(n);
  io.println("len=\(s.len(r)) n=\(s.split(r;"]").len)");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 23 | 181 | 76.21 | 63680 |
| `b` — avoid | 24 | 176 | 30000.00 | pending |
| `c` — canonical | 23 | 200 | 69.14 | 55600 |

## Errors (`err`)

This family covers error unions and early exit.

### err-propagate

Call a T!Err function and propagate its error to the caller.

Only inside a function that itself returns T!Err (else E3020). Form b re-wraps the error by hand; `!` is the direct form.

**Write this** — `let v=chk(x)!$myerr` (`patterns/err-propagate/a.tk`):

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

**Not this** — `mt … {$ok:v v;$err:e <$myerr{…}}` re-raise (`patterns/err-propagate/b.tk`):

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
  let v=mt chk(x){$ok:v v;$err:e <$myerr{$bad:true}};
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 21 | 49 | 63.67 | 73184 |
| `b` — avoid | 24 | 79 | 66.54 | 73168 |

### err-default

Turn a T!Err result into a plain value with a default on error.

Consuming an error union where a sentinel is acceptable. Form b avoids the union by re-checking the precondition and calling a non-failing helper (duplicates the check, only possible when the failure condition is known to the caller). Form c is the single-use-let anti-pattern (mined rank 33).

**Write this** — precondition if + plain call (`patterns/err-default/b.tk`):

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

**Not this** — `let r=mt …; <r` (`patterns/err-default/c.tk`):

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
  let r=mt chk(x){$ok:v v;$err:e -1};
  <r
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 11 | 46 | 115.74 | 144896 |
| `b` — canonical | 12 | 41 | 64.72 | 1456 |
| `c` — avoid | 13 | 54 | 115.62 | 144896 |

`tkc --lint` reports the non-canonical forms as `single-use-let`.

### err-validate-early

Validate arguments and return sentinels before the main computation.

Guard clauses at the top of a function. Form b nests each guard in the previous one's else; form c is a single expression-if chain — equal in tokens to a, slightly longer in bytes.

**Write this** — sequential guard returns `if(..){<-1};` (`patterns/err-validate-early/a.tk`):

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

**Not this** — nested if/el with returns (`patterns/err-validate-early/b.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(a:i64;b:i64):i64{
  if(a<0){<-1}el{if(b==0){<-2}el{<a/b}}
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 13 | 55 | 79.03 | 1440 |
| `b` — avoid | 15 | 61 | 77.24 | 1440 |
| `c` — avoid | 13 | 58 | 76.60 | 1440 |

## Parsing (`parse`)

This family covers turning text into values.

### parse-json

Read typed fields (i64, str) out of a JSON document.

Any structured JSON input. json.dec + typed accessors is the only form that is correct on reordered keys, whitespace, escapes and nesting; the hand-scan forms are sketches that only work on a fixed layout. Runtime gap: json.dec allocates a heap Json per decode; json.i64 returns the 0 sentinel as $err (a field whose value is 0 is indistinguishable from a missing field); json.arr always returns $err on 2.8.0 (131.30 gap).

**Write this** — json.dec + json.i64/json.str via mt (`patterns/parse-json/a.tk`):

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

**Not this** — indexof/slice hand scan (sketch) (`patterns/parse-json/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=json:std.json;
f=pat(src:str):i64{
  let pa=s.indexof(src;"\"a\":")+4;
  let a=mt s.toint(s.slice(src;pa;s.indexof(src;","))) {$ok:v v;$err:e <-1};
  let pb=s.indexof(src;"\"b\":\"")+5;
  <a+s.len(s.slice(src;pb;s.len(src)-2))
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

**Hot path** — per-char charcode scan loop (sketch) (`patterns/parse-json/c.tk`). Choose it when the layout is fixed and machine-generated, the parse sits in a loop body executed > 100k times AND profiling shows `json.dec` on the hot path; never for external input (hand scans are wrong on reordered or escaped JSON). The charcode scan measured 50.6 ms against 66.6 ms for `json.dec` (-24%), for 11 tokens more.

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=json:std.json;
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 40 | 161 | 67.18 | 33696 |
| `b` — avoid | 46 | 198 | 66.45 | 33648 |
| `c` — hot path | 51 | 244 | 56.18 | 25600 |

`tkc --lint` reports the non-canonical forms as `hand-rolled-parser`.

### parse-csv-line

Split CSV text into rows of fields.

b (s.split on newline then comma) is only correct for unquoted fields; a (csv.parse) is required whenever fields may be quoted or contain separators/newlines (RFC 4180). csv.parse takes [byte] so the input needs s.bytes(txt); rows are csvrow structs, read fields via rows.get(r).fields. Interpolating a csv field element prints a pointer (127.7 class) - print it directly or use s.len.

**Write this** — s.split lines then s.split "," (`patterns/parse-csv-line/b.tk`):

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

**Not this** — csv.parse(s.bytes(txt)) rows .fields (`patterns/parse-csv-line/a.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=csv:std.csv;
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
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let b=s.builder();
  lp(let i=0;i<n;i=i+1){s.add(b;"aa,bbb,c\(i%10)\n")};
  io.println("t=\(pat(s.build(b)))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 54 | 209 | 59.07 | 92992 |
| `b` — canonical | 50 | 200 | 58.58 | 56528 |

### parse-delim-split

Split one line on a single-character delimiter into a field array.

Any delimiter split where empty fields must be preserved. s.split is 5-7x fewer tokens than either manual form and is the only one that does not re-scan the string; b re-slices the remainder every iteration (quadratic in fields x line length).

**Write this** — s.split(line;",") (`patterns/parse-delim-split/a.tk`):

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

**Not this** — indexof + slice loop (`patterns/parse-delim-split/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 7 | 40 | 60.06 | 53808 |
| `b` — avoid | 50 | 219 | 79.77 | 90048 |
| `c` — avoid | 35 | 185 | 75.98 | 90016 |

`tkc --lint` reports the non-canonical forms as `hand-rolled-parser`.

### parse-fields

Split a line on whitespace runs, dropping empty fields.

Whitespace tokenising (A6). s.fields is canonical; b (split on " " + skip empties) is what LLMs write when they forget fields exists. Direct reads of fields elements are correct; interpolating an element prints a pointer until 127.9 lands - use io.println(x) or s.len.

**Write this** — s.fields(line) (`patterns/parse-fields/a.tk`):

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

**Not this** — charcode scan + slice loop (`patterns/parse-fields/c.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 9 | 37 | 97.69 | 97952 |
| `b` — avoid | 22 | 132 | 211.39 | 299232 |
| `c` — avoid | 49 | 230 | 169.29 | 178400 |

`tkc --lint` reports the non-canonical forms as `hand-rolled-parser`.

### parse-int

Parse a decimal integer string, yielding a default on failure.

Any int parse. mt s.toint is 9 tokens vs 25 for a digit loop and handles sign/overflow. Note s.toint result 0 is indistinguishable from $err under mt on 2.8.0 (0 sentinel) when the text is "0".

**Write this** — mt s.toint(txt) {$ok:v v;$err:e -1} (`patterns/parse-int/a.tk`):

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

**Hot path** — charcode digit loop (`patterns/parse-int/b.tk`). Choose it when millions of fields are parsed and the text is known to be unsigned ASCII digits: the charcode loop measured 72.7 ms against 77.4 ms (+6.4%) — 16 tokens more for 6%, and it handles neither sign nor overflow.

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+pat("\(i*7)")+pat("x\(i)")};
  io.println("t=\(t)");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 9 | 54 | 99.10 | 49680 |
| `b` — hot path | 25 | 144 | 91.47 | 49680 |

`tkc --lint` reports the non-canonical forms as `hand-rolled-parser`.

### parse-kv-lines

Turn key=value lines into a str->str map.

Config-style text. Both forms need a seeded map literal (mut.@() as a map crashes with RT003 on set - 131.30 gap) so the sentinel key "" is present; keys() on a map returned from a user function fails to link (undefined _keys), so the checksum is computed in a helper that receives the map as a typed parameter. String values read back from the map interpolate as pointers (127.7 class); use io.println(v) or s.len(v).

**Write this** — s.split lines -> s.split "=" -> m.set (`patterns/parse-kv-lines/a.tk`):

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

**Hot path** — s.indexof "=" + s.slice -> m.set (`patterns/parse-kv-lines/b.tk`). Choose it when millions of lines are parsed: `b` allocates two strings per line where `a` allocates a 2-element array plus two strings, and measured 67.3 ms against 74.7 ms (-10%), for 12 tokens more.

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
    let l=ls.get(i);
    let p=s.indexof(l;"=");
    if(p>0){m=m.set(s.slice(l;0;p);s.slice(l;p+1;s.len(l)))}
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 33 | 173 | 85.07 | 80560 |
| `b` — hot path | 45 | 195 | 76.78 | 64080 |

## Iteration (`iter`)

This family covers traversals.

### iter-map

Transform every element of an array with a per-element function.

Pure per-element transforms with a named function (&f). Keep a lp when the body is stateful or needs the index.

**Write this** — xs.map(&dbl) (`patterns/iter-map/a.tk`):

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

**Hot path** — index loop + append (`patterns/iter-map/b.tk`). Choose it when not established: the index loop measured 118.0 ms against 128.4 ms for `xs.map(&dbl)` (+8.9%) at N=16000, but the ordering is not reproducible — the same compiler binary measured `a` fastest 70 minutes earlier on the same machine (112.1 ms vs 117.1 ms, run 20260919-140946), so the gap is inside this machine's run-to-run variance (measured_at.load_warning). Both forms are the same big-O and allocate identically (16.2k calls, 977 MB). Do not act on this hot_path until a loadavg <= 2 re-measure confirms it..

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=dbl(x:i64):i64{<x*2};
f=pat(xs:@i64):@i64{
  let r=mut.@();
  lp(let i=0;i<xs.len;i=i+1){r=r.append(xs.get(i)*2)};
  <r
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 13 | 34 | 128.44 | 1111696 |
| `b` — hot path | 16 | 89 | 117.98 | 1111856 |

`tkc --lint` reports the non-canonical forms as `loop-is-map`.

### iter-filter

Keep the elements that satisfy a predicate.

Predicate is a named bool function. Element interpolation of the result: safe for @i64; for @str the filter result is untagged (127.10 class) - read elements directly.

**Write this** — xs.filter(&isev) (`patterns/iter-filter/a.tk`):

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

**Not this** — index loop + if + append (`patterns/iter-filter/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isev(x:i64):bool{<x%2==0};
f=pat(xs:@i64):@i64{
  let r=mut.@();
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%2==0){r=r.append(xs.get(i))}
  };
  <r
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 14 | 38 | 138.70 | 1111616 |
| `b` — avoid | 18 | 107 | 146.20 | 1111712 |

`tkc --lint` reports the non-canonical forms as `loop-is-filter`.

### iter-filter-sum

Sum the elements that satisfy a predicate.

Filter-then-aggregate. c folds the predicate into a single reduce (one pass, no intermediate array); a allocates the filtered array; b is the inline single loop. Likely hot-path split: b has no indirect call per element.

**Write this** — xs.reduce(0;&addev) (predicate folded) (`patterns/iter-filter-sum/c.tk`):

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

**Not this** — xs.filter(&p).reduce(0;&add) (`patterns/iter-filter-sum/a.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isev(x:i64):bool{<x%2==0};
f=add(a:i64;b:i64):i64{<a+b};
f=addev(a:i64;x:i64):i64{<if(x%2==0){a+x}el{a}};
f=pat(xs:@i64):i64{
  <xs.filter(&isev).reduce(0;&add)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

**Hot path** — single loop with if (`patterns/iter-filter-sum/b.tk`). Choose it when arrays run past ~100k elements or the loop body is executed > 1k times: the inline loop avoids the per-element indirect call of `reduce` and measured 54.7 ms against 63.5 ms (-14%), for 2 tokens more.

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isev(x:i64):bool{<x%2==0};
f=add(a:i64;b:i64):i64{<a+b};
f=addev(a:i64;x:i64):i64{<if(x%2==0){a+x}el{a}};
f=pat(xs:@i64):i64{
  let t=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%2==0){t=t+xs.get(i)}
  };
  <t
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 21 | 52 | 100.73 | 324912 |
| `b` — hot path | 19 | 96 | 78.27 | 260880 |
| `c` — canonical | 17 | 40 | 99.09 | 260880 |

### iter-find-first

Return the first element satisfying a predicate, or a default.

Early-exit search. a returns directly from inside the loop (no flag, no br); b scans everything and allocates; c is the mut-flag scan. xs.find(v) is by value, not predicate, and segfaults on arrays (127.11) so it is not a candidate here (see coll-membership).

**Write this** — loop with direct < return (`patterns/iter-find-first/a.tk`):

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

**Not this** — filter(&p) then .get(0) guarded by .len (`patterns/iter-find-first/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=big(x:i64):bool{<x>990};
f=pat(xs:@i64):i64{
  let hits=xs.filter(&big);
  <if(hits.len>0){hits.get(0)}el{-1}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 17 | 81 | 68.47 | 260880 |
| `b` — avoid | 22 | 79 | 83.76 | 262064 |
| `c` — avoid | 22 | 99 | 70.65 | 260880 |

`tkc --lint` reports the non-canonical forms as `scan-without-break`.

### iter-count

Count the elements satisfying a predicate.

No count/any/all combinator exists on 2.8.0 (ABSENT - 131.30 gap: xs.count(&p) would be ~9 tokens). a allocates the filtered array only to read .len; b is the plain loop; c is a reduce with the predicate folded.

**Write this** — loop counter (`patterns/iter-count/b.tk`):

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

**Not this** — xs.reduce(0;&cntev) (`patterns/iter-count/c.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isev(x:i64):bool{<x%2==0};
f=cntev(a:i64;x:i64):i64{<if(x%2==0){a+1}el{a}};
f=pat(xs:@i64):i64{
  <xs.reduce(0;&cntev)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 15 | 41 | 107.70 | 324928 |
| `b` — canonical | 15 | 88 | 88.02 | 260880 |
| `c` — avoid | 16 | 40 | 107.11 | 260880 |

### iter-nested-early-exit

Find the first (i,j) pair in a nested loop and stop.

Nested search inside a function: return directly with < from the inner loop (a) instead of a found-flag plus two br (b). Only applies when the search is its own function; inline searches in main still need br.

**Write this** — nested lp + direct < return (`patterns/iter-nested-early-exit/a.tk`):

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

**Not this** — mut flag + br in both loops (`patterns/iter-nested-early-exit/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 27 | 124 | 81.50 | 17888 |
| `b` — avoid | 35 | 153 | 79.07 | 17888 |

`tkc --lint` reports the non-canonical forms as `flag-break-is-return`.

## Collections (`coll`)

This family covers collection queries.

### coll-lookup-default

Read a map value, substituting a default when the key is missing.

str-keyed maps. m.get(k) on a missing key returns 0 and mt treats the 0 sentinel as $err, so BOTH forms conflate a stored 0 with "missing"; there is no m.has (m.contains silently returns 0 for present keys) - 131.30 gap. The mt arm cannot appear inside a binary expression (t=t+mt ... is E2002); bind it with let first.

**Write this** — let v=mt m.get(k) {$ok:x x;$err:e d} (`patterns/coll-lookup-default/a.tk`):

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

**Not this** — let v=m.get(k); if(v==0){d}el{v} (`patterns/coll-lookup-default/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(m:@(str:i64);ks:@str):i64{
  let t=mut.0;
  lp(let i=0;i<ks.len;i=i+1){
    let v=m.get(ks.get(i));
    let d=if(v==0){7}el{v};
    t=t+d
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 26 | 125 | 118.37 | 1114368 |
| `b` — avoid | 28 | 127 | 120.26 | 1114352 |

### coll-membership

Count how many query values occur in a reference array.

Repeated membership tests. a builds a str-keyed map once (int-keyed map literals @(0:1) segfault on .set - 131.30 gap - so keys are "\(x)"). Forms b (hand-rolled linear scan per query) and c (`xs.contains(q)` per query, unblocked when 127.11 closed) are both O(N*Q): measured 15.3 s and 15.0 s against 64 ms for a at N=256000, and both time out at 4N. c is token-best (18 vs 45) but the quadratic class is never taught as the default, so a stays canonical.

**Write this** — map-as-set + mt get (`patterns/coll-membership/a.tk`):

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

**Not this** — linear scan with br per query (`patterns/coll-membership/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(xs:@i64;qs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<qs.len;i=i+1){
    lp(let j=0;j<xs.len;j=j+1){
      if(xs.get(j)==qs.get(i)){c=c+1;br}
    }
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 45 | 205 | 64.25 | 44048 |
| `b` — avoid | 24 | 133 | 15348.13 | 9888 |
| `c` — avoid | 18 | 104 | 14984.45 | 9888 |

### coll-sort-take

Sort ascending and take the first k elements.

Top-k. c (`xs.sort(&cmp).slice(0;k)`) is the natural form and was blocked until 127.11 closed; since 2026-09-19 it runs, matches its siblings, is token-best (18 vs 24/52) and is runtime-tied with a and b, so it is canonical. a sorts then copies k elements by hand; b does k selection passes (O(N*k), linear for fixed k but 2.9x the tokens).

**Write this** — xs.sort(&cmp).slice(0;k) (127.11 fixed; unblocked 131.25) (`patterns/coll-sort-take/c.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=cmp(a:i64;b:i64):i64{<a-b};
f=pat(xs:@i64;k:i64):@i64{
  <xs.sort(&cmp).slice(0;k)
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

**Not this** — xs.sort(&cmp) + append first k (`patterns/coll-sort-take/a.tk`):

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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 24 | 109 | 117.32 | 1111680 |
| `b` — avoid | 52 | 207 | 115.75 | 1112848 |
| `c` — canonical | 18 | 52 | 119.00 | 1111680 |

### coll-group-by

Group values by a derived string key into a map of arrays.

Grouping. a: map of arrays with mt-get-or-empty then set; b: parallel keys/groups arrays with a linear key scan (O(N*K)). Map literal must be seeded (mut.@("":@())) because an empty map literal crashes on set (131.30 gap); the checksum is computed inside pat because .keys() on a map returned from a user function fails to link.

**Write this** — map of arrays via mt get-or-@() + set (`patterns/coll-group-by/a.tk`):

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

**Not this** — parallel key array + nested arrays (`patterns/coll-group-by/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%1000)};
  io.println("t=\(pat(xs))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 64 | 279 | 165.85 | 1479184 |
| `b` — avoid | 89 | 408 | 162.90 | 1479184 |

### coll-reverse

Produce the reversed copy of an array.

No reverse combinator exists (131.30 gap). a appends from the end; b copies and swaps in place; c prepends with @(x)+r and is quadratic (each + copies the accumulator) - token-tied with a but worse-bigO, so it must never be taught.

**Write this** — loop from end + append (`patterns/coll-reverse/a.tk`):

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

**Not this** — copy + set swap to the middle (`patterns/coll-reverse/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 16 | 90 | 117.23 | 1111840 |
| `b` — avoid | 24 | 130 | 327.17 | 3167552 |
| `c` — avoid | 16 | 82 | 223.81 | 2222192 |

`tkc --lint` reports the non-canonical forms as `quadratic-prepend`.

### coll-swap

Swap two elements of an array.

Any element swap (sort inner loops). Chained r=r.set(i;r.get(j)).set(j;r.get(i)) is correct under value semantics (the second r.get reads the original) and one token cheaper than the tmp form the card shows; verified byte-identical output.

**Write this** — chained .set().set() (`patterns/coll-swap/b.tk`):

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

**Not this** — let tmp + two set statements (card form) (`patterns/coll-swap/a.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — avoid | 26 | 126 | 85.62 | 807040 |
| `b` — canonical | 25 | 114 | 85.30 | 807040 |

`tkc --lint` reports the non-canonical forms as `swap-tmp-let`.

### coll-dedupe

Count (or collect) the distinct values of an array.

Dedupe. b (nested scan over the unique list) is token-best but O(N*U) - worse-bigO when uniques grow with N - so the map-as-set form is canonical without a hot path. Int-keyed maps segfault (131.30 gap), so the set key is "\(x)".

**Write this** — str-keyed map-as-set + mt get (`patterns/coll-dedupe/a.tk`):

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

**Not this** — nested scan over unique list (`patterns/coll-dedupe/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
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
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let xs=mut.@();
  lp(let i=0;i<n;i=i+1){xs=xs.append((i*7)%(n*3/4))};
  io.println("u=\(pat(xs))");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 41 | 187 | 53.16 | 36848 |
| `b` — avoid | 34 | 182 | 6064.13 | 9376 |

`tkc --lint` reports the non-canonical forms as `quadratic-dedupe`.

## Program boundary (`cli`)

This family covers argv, stdin and printing results.

### cli-argv

Read the first program argument with a default when absent.

args.get(n) returns $err past the end, so mt is a one-expression form; the count check is 7 tokens more. The args.get result interpolates as a pointer (127.7 class) - print it directly or take s.len. args.count() includes the program name.

**Write this** — mt args.get(1) {$ok:v v;$err:e "default"} (`patterns/cli-argv/a.tk`):

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

**Not this** — if(args.count()>1){args.get(1)}el{"default"} (`patterns/cli-argv/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=args:std.args;
f=pat():str{
  <if(args.count()>1){args.get(1)}el{"default"}
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+s.len(pat())};
  io.println("t=\(t)");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 8 | 53 | 64.43 | 1408 |
| `b` — avoid | 15 | 58 | 64.52 | 1408 |

### cli-flag-filter

Drop --flag arguments from an argv array.

The D-CLI "nonflags" shape: filter with a named predicate vs loop+if+append. The loop form builds with r=r+@(x) because an .append-built @str interpolates as pointers until 127.10 lands; the filter result is read directly in the fixture.

**Write this** — av.filter(&notflag) (`patterns/cli-flag-filter/a.tk`):

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

**Not this** — loop + if + r=r+@(x) (`patterns/cli-flag-filter/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=notflag(x:str):bool{<!s.startswith(x;"--")};
f=pat(av:@str):@str{
  let r=mut.@();
  lp(let i=0;i<av.len;i=i+1){
    if(!s.startswith(av.get(i);"--")){r=r+@(av.get(i))}
  };
  <r
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 14 | 41 | 121.60 | 1113008 |
| `b` — avoid | 18 | 117 | 170.89 | 1622656 |

`tkc --lint` reports the non-canonical forms as `loop-is-filter`.

### cli-print-results

Format a labelled result as "key=value".

The 8.5% io.println(show(x)) shape (131.3). For i64/str values interpolation is the best form today; there is no show for bool (prints 1/0 - 127.15) or for arrays/maps (131.30 stdlib gap - a stdlib show/fmt would replace the hand formatters). Values derived from method calls interpolate as pointers (127.7).

**Write this** — "\(k)=\(v)" interpolation (`patterns/cli-print-results/a.tk`):

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

**Not this** — s.builder add/add/add (`patterns/cli-print-results/c.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(k:str;v:i64):str{
  let b=s.builder();
  s.add(b;k);
  s.add(b;"=");
  s.add(b;s.fromint(v));
  <s.build(b)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+s.len(pat("r\(i%5)";i*13))};
  io.println("t=\(t)");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 10 | 36 | 89.91 | 49664 |
| `b` — avoid | 15 | 63 | 86.21 | 57712 |
| `c` — avoid | 16 | 99 | 92.37 | 73808 |

`tkc --lint` reports the non-canonical forms as `nested-concat`.

## Decomposition (`fn`)

This family covers helpers, chaining and recursion.

### fn-helper-vs-inline

Test a compound condition inside a loop: named helper or inline expression.

Protocol region counts only pat: the helper call (16) beats the inline condition (21) but the helper itself costs 14 proxy8k tokens (isok: min_bytes 35), so whole-program the inline form wins for a single call site; a helper pays for itself from the second call site. -O2 is expected to inline the call.

**Write this** — call named helper isok(x) (`patterns/fn-helper-vs-inline/a.tk`):

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

**Not this** — inline x%3==0&&x%5!=0 (`patterns/fn-helper-vs-inline/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=isok(x:i64):bool{<x%3==0&&x%5!=0};
f=pat(xs:@i64):i64{
  let c=mut.0;
  lp(let i=0;i<xs.len;i=i+1){
    if(xs.get(i)%3==0&&xs.get(i)%5!=0){c=c+1}
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 16 | 89 | 86.08 | 260880 |
| `b` — avoid | 21 | 104 | 85.54 | 260864 |

### fn-chain-vs-let

Read one field of a split line.

Single-use intermediates. Chained postfix (a) and the two-let form (b) count the same proxy8k tokens (11) - the hostile proxy has cheap merges for let - but a is 34 bytes shorter (min_bytes tie-break) and 3 tokens shorter under v03.

**Write this** — s.len(s.split(line;",").get(1)) (`patterns/fn-chain-vs-let/a.tk`):

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

**Not this** — let parts=...; let second=parts.get(1); s.len(second) (`patterns/fn-chain-vs-let/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(line:str):i64{
  let parts=s.split(line;",");
  let second=parts.get(1);
  <s.len(second)
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+pat("ab,c\(i),de")};
  io.println("t=\(t)");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 11 | 53 | 51.90 | 45840 |
| `b` — avoid | 11 | 87 | 52.06 | 45856 |

`tkc --lint` reports the non-canonical forms as `single-use-let`.

### fn-recursion-vs-loop

Sum the decimal digits of an integer.

Depth-bounded recursion (<= 19 frames). Recursion is 5 tokens cheaper; the loop avoids call overhead. Never recurse on unbounded depth (no TCO; stack).

**Write this** — expression-if recursion (`patterns/fn-recursion-vs-loop/a.tk`):

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

**Hot path** — lp with mut accumulator (`patterns/fn-recursion-vs-loop/b.tk`). Choose it when the call sits in a loop executed > 1M times, or the recursion depth is data-dependent (stack safety): the `lp` accumulator measured 77.2 ms against 112.6 ms (-31%), for 5 tokens more.

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(x:i64):i64{
  let t=mut.0;
  let v=mut.x;
  lp(let k=0;v>0;k=k+1){t=t+v%10;v=v/10};
  <t
};
f=main():i64{
  let n=env.getint("PAT_N";1000);
  let t=mut.0;
  lp(let i=0;i<n;i=i+1){t=t+pat(i*7919+123456789)};
  io.println("t=\(t)");
  <0
};
```

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 15 | 48 | 80.76 | 1424 |
| `b` — hot path | 20 | 83 | 58.07 | 1424 |

## Files (`io`)

This family covers reading and writing files.

### io-read-lines

Read a file and split it into lines.

Whole-file reads. No file.readlines exists (131.30 gap); s.split(txt;"\n") yields a trailing "" when the file ends in a newline - both forms keep it. file.read needs --allow-read (fixtures build with --allow-all).

**Write this** — mt file.read + s.split "\n" (`patterns/io-read-lines/a.tk`):

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

**Not this** — mt file.read + charcode scan + slice per line (`patterns/io-read-lines/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 19 | 87 | 84.12 | 83152 |
| `b` — avoid | 45 | 228 | 30000.00 | pending |

`tkc --lint` reports the non-canonical forms as `hand-rolled-parser`.

### io-write-accumulate

Write N lines to a file.

Append-per-line is token-best (23 vs 27) but reopens the file on every call (a syscall per line, same asymptotic class); the protocol therefore makes it canonical with a hot path - flagged for owner review: a 10-100x constant-factor loss with identical bigO is exactly the case rule 6.4 does not catch. Fixture deletes the file first because file.append accumulates across runs.

**Write this** — s.builder then one file.write (`patterns/io-write-accumulate/a.tk`):

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

**Not this** — file.append per line (`patterns/io-write-accumulate/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=file:std.file;
f=pat(path:str;n:i64):i64{
  lp(let i=0;i<n;i=i+1){
    let ok=mt file.append(path;"row \(i)\n") {$ok:v 1;$err:e <0}
  };
  <1
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

| form | proxy tokens | `--min` bytes | wall ms | RSS KB |
|---|---|---|---|---|
| `a` — canonical | 27 | 139 | 72.48 | 53760 |
| `b` — avoid | 23 | 111 | 36823.43 | 28528 |

`tkc --lint` reports the non-canonical forms as `append-in-loop`.

## Where to go next

The [normative catalogue](/docs/spec/patterns-v0.4/) carries every measured column, the sources each pattern was mined from and the open compiler caveats; the [idiom standard](/docs/spec/idiom-v0.4/) states the same rules as prose. Run `tkc --lint` on your own programs to have the compiler point at non-canonical forms.
