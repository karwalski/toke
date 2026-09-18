---
title: Lesson 11 — Patterns and Efficiency
slug: 11-patterns-and-efficiency
section: learn
order: 11
---

> **GENERATED** by `scripts/patterns/render_catalogue.py guide` from `patterns/catalogue.json` (sha256 `fa57606a3a04`). Do not hand-edit: change the catalogue, run `make render-patterns`; `make check-patterns` fails CI on drift.

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

**Not this** — mut-flag then if/el assigns (`patterns/cond-bind-if/b.tk`):

```toke
m=main;
i=io:std.io;
i=env:std.env;
f=pat(x:i64):i64{
  let g=mut.0;
  if(x>0){g=1}el{g=2};
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
| `a` — avoid | 17 | 46 | pending | pending |
| `b` — avoid | 20 | 56 | pending | pending |
| `c` — canonical | 15 | 43 | pending | pending |

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
| `a` — canonical | 21 | 63 | pending | pending |
| `b` — avoid | 22 | 65 | pending | pending |
| `c` — avoid | 28 | 74 | pending | pending |

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
| `a` — canonical | 10 | 45 | pending | pending |
| `b` — avoid | 17 | 68 | pending | pending |
| `c` — avoid | 13 | 50 | pending | pending |

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
| `a` — canonical | 11 | 65 | pending | pending |
| `b` — avoid | 18 | 76 | pending | pending |
| `c` — avoid | 17 | 62 | pending | pending |

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
| `a` — canonical | 8 | 48 | pending | pending |
| `c` — avoid | 13 | 59 | pending | pending |

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
| `a` — canonical | 16 | 76 | pending | pending |
| `b` — avoid | 16 | 39 | pending | pending |

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
| `a` — canonical | 16 | 88 | pending | pending |
| `b` — avoid | 15 | 41 | pending | pending |
| `c` — avoid | 16 | 39 | pending | pending |

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
| `a` — hot path | 18 | 99 | pending | pending |
| `b` — avoid | 18 | 45 | pending | pending |
| `c` — canonical | 14 | 41 | pending | pending |

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
| `a` — canonical | 13 | 74 | pending | pending |
| `b` — avoid | 13 | 69 | pending | pending |
| `c` — avoid | 27 | 95 | pending | pending |

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
| `a` — avoid | 50 | 180 | pending | pending |
| `b` — canonical | 48 | 209 | pending | pending |

### acc-dedupe

Remove duplicate values from an @i64 (order not significant).

Dedupe where result order is free (checksum is len+sum). Form a (array `.contains`) is the natural form but segfaults on 2.8.0 (127.11). Form d keys a map by the interpolated value: one interpolation alloc per element and a linear strcmp scan of the seen-set, so it is quadratic like b.

The preferred way to write this — `if(!out.contains(v)){out=out.append(v)}` — is blocked on compiler story 127.11, so today's canonical form is the best form that works. The blocked form, for reference (not compiled):

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

**Write this** — sort a copy, append when != previous (`patterns/acc-dedupe/c.tk`):

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

**Not this** — seen-map keyed by `"\(v)"` (`patterns/acc-dedupe/d.tk`):

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
| `b` — avoid | 25 | 186 | pending | pending |
| `c` — canonical | 31 | 150 | pending | pending |
| `d` — avoid | 31 | 176 | pending | pending |

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
| `a` — avoid | 15 | 96 | pending | pending |
| `b` — canonical | 13 | 105 | pending | pending |
| `c` — avoid | 15 | 114 | pending | pending |

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
| `a` — canonical | 13 | 47 | pending | pending |
| `b` — avoid | 15 | 62 | pending | pending |
| `c` — avoid | 20 | 95 | pending | pending |

`tkc --lint` reports the non-canonical forms as `string-concat-chain`.

### str-num-format

Convert an i64 to its decimal string.

Number to text with no width/precision. All three forms reach tk_str_fromi64 (1 alloc). `n as str` is accepted by 2.8.0 although the card lists only numeric casts.

**Write this** — interpolation `"\(n)"` (`patterns/str-num-format/a.tk`):

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

**Not this** — `s.fromint(n)` (`patterns/str-num-format/b.tk`):

```toke
m=main;
i=io:std.io;
i=s:std.str;
i=env:std.env;
f=pat(n:i64):str{
  <s.fromint(n)
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
| `a` — canonical | 7 | 25 | pending | pending |
| `b` — avoid | 8 | 31 | pending | pending |
| `c` — avoid | 8 | 27 | pending | pending |

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

**Not this** — `s.concat(s.repeat(" ";p);d)` (`patterns/str-repeat-pad/c.tk`):

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
| `a` — canonical | 25 | 97 | pending | pending |
| `b` — avoid | 25 | 126 | pending | pending |
| `c` — avoid | 28 | 102 | pending | pending |
| `d` — avoid | 28 | 99 | pending | pending |

### str-array-render

Collect N labels into a @str, then render each one.

The collect-then-format shape of CLI/report tasks. Interpolating an element of a `.append`-built `mut.@()` prints an address on 2.8.0 (127.10: the array is never inferred `@str`), so the natural form a is blocked; form b uses the only append form that tags the array (`+@()`) but copies the array every iteration; form c keeps `.append` and reads elements directly into the builder.

The preferred way to write this — `.append`-built, interpolated read `"[\(labels.get(i))]"` — is blocked on compiler story 127.10, so today's canonical form is the best form that works. The blocked form, for reference (not compiled):

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
| `b` — avoid | 24 | 176 | pending | pending |
| `c` — canonical | 23 | 200 | pending | pending |

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
| `a` — canonical | 21 | 49 | pending | pending |
| `b` — avoid | 24 | 79 | pending | pending |

### err-default

Turn a T!Err result into a plain value with a default on error.

Consuming an error union where a sentinel is acceptable. Form b avoids the union by re-checking the precondition and calling a non-failing helper (duplicates the check, only possible when the failure condition is known to the caller). Form c is the single-use-let anti-pattern (mined rank 33).

**Write this** — `<mt chk(x){$ok:v v;$err:e -1}` (`patterns/err-default/a.tk`):

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
| `a` — canonical | 11 | 46 | pending | pending |
| `b` — avoid | 12 | 41 | pending | pending |
| `c` — avoid | 13 | 54 | pending | pending |

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
| `a` — canonical | 13 | 55 | pending | pending |
| `b` — avoid | 15 | 61 | pending | pending |
| `c` — avoid | 13 | 58 | pending | pending |

## Where to go next

The [normative catalogue](/docs/spec/patterns-v0.4/) carries every measured column, the sources each pattern was mined from and the open compiler caveats; the [idiom standard](/docs/spec/idiom-v0.4/) states the same rules as prose. Run `tkc --lint` on your own programs to have the compiler point at non-canonical forms.
