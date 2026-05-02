---
title: Migration Guide — Legacy to Default Syntax
slug: migration
section: reference
order: 8
---

This guide is for users who wrote toke code under the early 80-char profile, or who are reading documentation that predates the default syntax decision. It covers every syntactic change between the two profiles.

## Background

toke has two syntax profiles:

- **Default (55-char)** — the current standard, adopted after Gate 1. All new code and documentation uses this profile.
- **Legacy (80-char)** — the earlier profile, retained for backwards compatibility under the `--legacy` flag.

Run legacy code without changes:

```sh
tkc --legacy myfile.tk
```

The `--legacy` flag tells the compiler to accept the old syntax throughout that compilation unit. You cannot mix profiles within one file.

## Automated migration

> `tkc --migrate` (story 11.3.5) will automate this conversion. It is not yet implemented. Until then, use this guide to migrate by hand.

---

## Syntax changes: full reference

### Array literals

| Profile | Syntax |
|---------|--------|
| Legacy | `[1, 2, 3]` |
| Default | `@(1;2;3)` |

**Example — legacy:**

```toke-legacy
let nums = [1, 2, 3];
```

**Example — default:**

```toke
f=example():void{
  let nums=@(1;2;3);
};
```

The `@(...)` delimiter is shared with map literals. Elements are separated by semicolons, not commas.

---

### Array subscript

| Profile | Syntax |
|---------|--------|
| Legacy | `arr[0]` |
| Default | `arr.get(0)` |

**Example — legacy:**

```toke-legacy
let first = arr[0];
```

**Example — default:**

```toke
f=example():void{
  let arr=@(10;20;30);
  let first=arr.get(0);
};
```

`.get(n)` takes a `u64` index and returns the element at that position.

---

### Map literals

| Profile | Syntax |
|---------|--------|
| Legacy | `{"key": val}` |
| Default | `@("key":val)` |

**Example — legacy:**

```toke-legacy
let ages = {"alice": 30, "bob": 25};
```

**Example — default:**

```toke
f=example():void{
  let ages=@("alice":30;"bob":25);
};
```

Keys are always `$str`. Entries are separated by semicolons.

---

### Map subscript

| Profile | Syntax |
|---------|--------|
| Legacy | `map["key"]` |
| Default | `map.get("key")` |

**Example — legacy:**

```toke-legacy
let age = ages["alice"];
```

**Example — default:**

```toke
f=example():void{
  let ages=@("alice":30;"bob":25);
  let age=ages.get("alice");
};
```

---

### Mutable binding

| Profile | Syntax |
|---------|--------|
| Legacy | `let mut x = 0` |
| Default | `let x=mut.0` |

**Example — legacy:**

```toke-legacy
let mut count = 0;
count = count + 1;
```

**Example — default:**

```toke
f=example():void{
  let count=mut.0;
  count=count+1;
};
```

The initial value follows `mut.` directly. After declaration, reassignment uses bare `name=expr;`.

---

### Else branch

| Profile | Syntax |
|---------|--------|
| Legacy | `else` |
| Default | `el` |

**Example — legacy:**

```toke-legacy
if (x > 0) {
    < "positive"
} else {
    < "non-positive"
}
```

**Example — default:**

```toke
f=example(x:i64):$str{
  if(x>0){
    <"positive"
  }el{
    <"non-positive"
  };
};
```

`el` is the complete keyword — there is no space or brace before it.

---

### Loop

| Profile | Syntax |
|---------|--------|
| Legacy | `for (i=0; i<n; i++)` |
| Default | `lp(let i=0;i<n;i=i+1)` |

**Example — legacy:**

```toke-legacy
for (i = 0; i < n; i++) {
    process(items[i]);
}
```

**Example — default:**

```toke
f=process(x:i64):void{};
f=example():void{
  let items=@(1;2;3);
  let n=3;
  lp(let i=0;i<n;i=i+1){
    process(items.get(i));
  };
};
```

`lp` takes three semicolon-separated clauses in its parens: init, condition, step. The body follows in `{...}`.

---

### Function definition

| Profile | Syntax |
|---------|--------|
| Legacy | `fn name(p:T):R {` |
| Default | `f=name(p:T):R{` |

**Example — legacy:**

```toke-legacy
fn add(a: i64, b: i64): i64 {
    return a + b
}
```

**Example — default:**

```toke
f=add(a:i64;b:i64):i64{
  <a+b
};
```

Parameters are separated by semicolons in the default profile.

---

### Not-equal

| Profile | Syntax |
|---------|--------|
| Legacy | `a != b` |
| Default | `!(a=b)` |

**Example — legacy:**

```toke-legacy
if (x != 0) { ... }
```

**Example — default:**

```toke
f=example(x:i64):void{
  if(!(x=0)){};
};
```

In the default profile `=` is the equality operator. Negation wraps the whole comparison: `!(a=b)`.

---

### Return

| Profile | Syntax |
|---------|--------|
| Legacy | `return val` |
| Default | `<val` |

**Example — legacy:**

```toke-legacy
return result;
```

**Example — default:**

```toke
f=example(result:i64):i64{
  <result
};
```

`<` is the return operator. It does not require a semicolon when it is the last statement in a block.

---

### Named type declaration

| Profile | Syntax |
|---------|--------|
| Legacy | `type mytype` |
| Default | `t=$mytype{...}` |

**Example — legacy:**

```toke-legacy
type point {
    x: i64,
    y: i64,
}
```

**Example — default:**

```toke
t=$point{x:i64;y:i64};
```

All named types carry the `$` sigil. Fields are separated by semicolons.

---

## Quick reference table

| Feature | Legacy (80-char) | Default (55-char) |
|---------|-----------------|-------------------|
| Array literal | `[1, 2, 3]` | `@(1;2;3)` |
| Array subscript | `arr[0]` | `arr.get(0)` |
| Map literal | `{"key": val}` | `@("key":val)` |
| Map subscript | `map["key"]` | `map.get("key")` |
| Mutable binding | `let mut x = 0` | `let x=mut.0` |
| Else branch | `else` | `el` |
| Loop | `for (i=0; i<n; i++)` | `lp(let i=0;i<n;i=i+1)` |
| Function def | `fn name(p:T):R {` | `f=name(p:T):R{` |
| Not-equal | `a != b` | `!(a=b)` |
| Return | `return val` | `<val` |
| Named type | `type mytype` | `t=$mytype{...}` |

---

## Running legacy code without migrating

If you have existing legacy code and do not want to migrate immediately, pass `--legacy` to the compiler:

```sh
tkc --legacy build src/main.tk
tkc --legacy check src/
```