---
title: Default Syntax — Encoding Design
slug: phase2-overview
section: reference
order: 20
---

# Encoding Design

The default syntax is a reduced-character profile of the toke language designed for use with the purpose-built BPE tokenizer. It expresses the same semantics as the legacy profile but uses only **55 characters**, enabling significantly higher token density during LLM inference.

The default syntax is the production encoding. The legacy profile (80 characters) was used during corpus generation to leverage existing LLM tokenizers, and programs are mechanically transformed to the default syntax for training and inference.

:::note
Default syntax source is **not** valid legacy profile source, and vice versa. The compiler accepts `--profile1` (legacy) or `--profile2` (default) to select the active profile.
:::

## Why Two Profiles?

The legacy profile uses existing LLM tokenizers (like cl100k_base) which were not designed for toke. It needs uppercase letters for type names and brackets for arrays because these are what existing tokenizers handle well.

The default syntax drops uppercase letters entirely and replaces bracket-heavy syntax with sigils (`$` for types, `@` for arrays). A purpose-built tokenizer trained on toke source can merge these patterns into single vocabulary entries — `$user`, `$str`, `@(` — achieving 2.5-4x better token density than the legacy profile with cl100k_base.

## Character Set Comparison

| Class | Legacy (80 chars) | Default (55 chars) | Change |
|-------|--------------------|--------------------|--------|
| Lowercase | a-z (26) | a-z (26) | Same |
| Uppercase | A-Z (26) | — | Removed |
| Digits | 0-9 (10) | 0-9 (10) | Same |
| Symbols | 18 | 18 | `[` `]` removed; `$` `@` added |
| Reserved | — | `^` `~` (2) | Reserved for v0.2 |
| **Total** | **80** | **56** | **-24** |

## Transformation Rules

Every legacy profile program can be mechanically transformed to the default syntax. The transformation is deterministic and reversible.

### 1. Type sigil: uppercase becomes `$lowercase`

The legacy profile uses uppercase-initial identifiers for types. The default syntax prefixes them with `$` and lowercases.

<div class="hero-comparison">
<div>

**Legacy**
```
T=User{id:u64;name:Str};
T=ApiErr{NotFound:u64;Timeout:Str};
```

</div>
<div>

**Default**
```
t=$user{id:u64;name:$str};
t=$apierr{$notfound:u64;$timeout:$str};
```

</div>
</div>

This applies to all type positions: declarations, annotations, struct literals, match arms, and error types.

### 2. Array literals: `[...]` becomes `@(...)`

<div class="hero-comparison">
<div>

**Legacy**
```
let nums=[1;2;3];
let names=["alice";"bob"];
```

</div>
<div>

**Default**
```
let nums=@(1;2;3);
let names=@("alice";"bob");
```

</div>
</div>

### 3. Array indexing: `a[n]` becomes `a.get(n)`

<div class="hero-comparison">
<div>

**Legacy**
```
let first=arr[0];
let item=arr[i];
```

</div>
<div>

**Default**
```
let first=arr.get(0);
let item=arr.get(i);
```

</div>
</div>

All array indexing uses `.get(n)`.

### 4. Map types: `[K:V]` becomes `@(K:V)`

<div class="hero-comparison">
<div>

**Legacy**
```
let ages=[Str:i64]["alice":30;"bob":25];
```

</div>
<div>

**Default**
```
let ages:@($str:i64)=@("alice":30;"bob":25);
```

</div>
</div>

### 5. Identifiers

Legacy profile identifiers may begin with uppercase or lowercase. Default syntax identifiers are lowercase only. All user-defined identifiers that began with uppercase in the legacy profile are lowercased and prefixed with `$` in the default syntax.

## Complete Example

<div class="hero-comparison">
<div>

**Legacy**
```
M=fib;

F=fib(n:i64):i64{
  if(n<2){<n};
  <fib(n-1)+fib(n-2);
};

F=main():i64{
  <fib(10);
};
```

</div>
<div>

**Default**
```
m=fib;

f=fib(n:i64):i64{
  if(n<2){<n};
  <fib(n-1)+fib(n-2);
};

f=main():i64{
  <fib(10);
};
```

</div>
</div>

This example shows that even simple programs differ between profiles: the default syntax lowercases the `M=`, `F=` keywords to `m=`, `f=`. The bigger differences emerge in programs that use the type system and collections.

<div class="hero-comparison">
<div>

**Legacy**
```
M=api;
I=http:std.http;
I=json:std.json;

T=ApiErr{
  NotFound:u64;
  BadRequest:Str
};

F=handle(req:http.Req):http.Res!ApiErr{
  let id=json.dec(req.body)!ApiErr;
  let users=["alice":1;"bob":2];
  <http.Res.ok(json.enc(users));
};
```

</div>
<div>

**Default**
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
  let users:@($str:i64)=@("alice":1;"bob":2);
  <http.$res.ok(json.enc(users));
};
```

</div>
</div>

## Token Efficiency

The purpose-built default syntax tokenizer merges common patterns into single vocabulary entries:

| Pattern | Legacy tokens (cl100k) | Default tokens (toke BPE) |
|---------|------------------------|--------------------------|
| `$user` | 2-3 | 1 |
| `$str` | 2 | 1 |
| `@(` | 2 | 1 |
| `f=` | 2 | 1 |
| `!$err` | 3 | 1 |
| `<$res.ok` | 4-5 | 1-2 |

The target vocabulary size is 32,768 tokens. The tokenizer specification is finalised after legacy corpus generation is complete.

## What Stays the Same

Everything not listed above is identical between profiles:

- Keywords: `f`, `t`, `i`, `m`, `if`, `el`, `lp`, `br`, `let`, `mut`, `as`, `rt`, `mt`
- Operators: `+`, `-`, `*`, `/`, `<`, `>`, `=`, `!`, `|`
- Delimiters: `(`, `)`, `{`, `}`, `;`, `:`, `.`
- Control flow: `if(){}`, `el{}`, `lp(){}`, `br`
- Functions, modules, imports, error handling, match expressions
- The entire standard library API
- All semantic rules, type checking, and error codes

## See Also

- [Type System](/docs/reference/types/) — production type reference (default syntax notation)
- [Default Syntax Type Details](/docs/reference/phase2/types/) — type transformation details
- [Grammar](/docs/reference/grammar/) — production grammar reference (default syntax notation)
- [Default Syntax Grammar Details](/docs/reference/phase2/grammar/) — grammar transformation details
- [Design Principles](/about/design/) — why toke has two profiles
