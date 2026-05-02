---
title: Default Syntax — Type Encoding
slug: phase2-types
section: reference
order: 21
---

# Type Encoding

This page documents the default syntax type system. The default syntax uses the same semantic rules as the [legacy profile](/docs/reference/types/) but with a different surface syntax: all type names are lowercased and prefixed with `$`.

## Primitive Types

| Legacy | Default | Size | Range / Values |
|---------|---------|------|----------------|
| `i64` | `i64` | 64 bits | -2^63 to 2^63-1 |
| `u64` | `u64` | 64 bits | 0 to 2^64-1 |
| `f64` | `f64` | 64 bits | IEEE 754 double |
| `bool` | `bool` | 1 byte | `true`, `false` |
| `Str` | `$str` | pointer | Immutable UTF-8 string |
| `void` | `void` | 0 bytes | Unit type |

Lowercase primitives (`i64`, `u64`, `f64`, `bool`, `void`) are unchanged. Only `Str` becomes `$str` because it begins with uppercase in the legacy profile.

## Composite Types

### Arrays

| Aspect | Legacy | Default |
|--------|---------|---------|
| Type notation | `[T]` | `@T` or `@($type)` |
| Literal | `[1;2;3]` | `@(1;2;3)` |
| Empty literal | `[]` | `@()` |
| Index | `arr[i]` | `arr.get(i)` |
| Length | `arr.len` | `arr.len` |

```
let nums=@(1;2;3);
let first=nums.get(0);
let n=nums.get(i);
let size=nums.len;
```

### Maps

| Aspect | Legacy | Default |
|--------|---------|---------|
| Type notation | `[K:V]` | `@(K:V)` |
| Literal | `["a":1;"b":2]` | `@("a":1;"b":2)` |
| Operations | `.get()`, `.put()`, `.contains()`, `.delete()`, `.len` | Same |

```
let ages:@($str:i64)=@("alice":30;"bob":25);
let age=ages.get("alice");
```

### Error Unions

Identical syntax to the legacy profile, except type names use `$` prefix:

```
f=readfile(path:$str):$str!$fileerr{
  let content=file.read(path)!$fileerr;
  <content;
};
```

## Struct and Sum Types

Type declarations use `$` prefix for the type name and for any uppercase-initial field names (sum type variants):

<div class="hero-comparison">
<div>

**Legacy**
```
T=Point{x:f64;y:f64};
T=Shape{
  Circle:f64;
  Rect:Point
};
```

</div>
<div>

**Default**
```
t=$point{x:f64;y:f64};
t=$shape{
  $circle:f64;
  $rect:$point
};
```

</div>
</div>

Struct field access is unchanged (`p.x`, `p.y`). Match arms use the variant names:

```
mt s {
  $circle:r  3.14*r*r;
  $rect:p    p.x*p.y
};
```

## Special Types

### Pointers (FFI)

```
f=malloc(size:u64):*u8;
f=free(ptr:*u8):void;
```

Pointer types use the same `*T` syntax. Only the inner type is transformed if it is uppercase.

## Type Inference

All inference rules from the [legacy profile](/docs/reference/types/#type-inference) apply identically. The inferred types simply use default syntax notation:

| Expression | Inferred Type |
|------------|---------------|
| `42` | `i64` |
| `3.14` | `f64` |
| `"hello"` | `$str` |
| `true` | `bool` |
| `@(1;2;3)` | `@i64` |
| `@("a":1)` | `@($str:i64)` |
| `$point{x:0.0;y:0.0}` | `$point` |

## Cast Expressions

```
let x=42;
let y=x as f64;
let s:$str="hello";
```

The `as` keyword and all cast rules are identical to the legacy profile.

## See Also

- [Type System](/docs/reference/types/) — production type reference
- [Default Syntax Overview](/docs/reference/phase2/overview/) — complete profile comparison
- [Default Syntax Grammar](/docs/reference/phase2/grammar/) — production rule differences
