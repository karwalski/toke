---
title: Lesson 5 — Error Handling
slug: 05-errors
section: learn
order: 5
---

**Estimated time: ~25 minutes**

## Errors are values, not exceptions

toke has no exceptions, no try/catch, no stack unwinding. Every error is a value that flows through the type system. If a function can fail, its signature says so. If you call a fallible function, you must handle the error at the call site.

This is a deliberate design choice. Exceptions hide control flow. In a language designed for machine generation, hidden control flow means hidden bugs. toke makes every error path visible.

## Error types

Errors are defined as sum types -- tagged unions where each variant represents a distinct failure mode:

```
t=$matherr{
  $divbyzero:bool;
  $overflow:$str
};
```

Each variant has a name and a payload type. Use `bool` for variants that carry no meaningful data -- the value `true` is implicit.

Here is a more realistic error type:

```
t=$dberr{
  $connectionfailed:$str;
  $queryfailed:$str;
  $notfound:u64;
  $timeout:u32
};
```

## The `T!E` return type

When a function can fail, its return type includes the error type after `!`:

```
t=$matherr{
  $divbyzero:bool;
  $overflow:$str
};

f=divide(a:f64;b:f64):f64!$matherr{
  if(b=0.0){
    <$matherr{$divbyzero:true};
  };
  <a/b;
};
```

The signature `:f64!$matherr` means: "this function returns an `f64` on success, or a `$matherr` on failure." The error union is a tagged value with `$ok` and `$err` variants, matched with `mt result {$ok:v ...; $err:e ...}`.

A function without `!` in its return type is **total** -- it cannot fail and cannot contain error propagation. The compiler enforces this (E3001).

## Returning errors

To return an error, construct the error variant:

```
i=str:std.str;

t=$parseerr{
  $negativeage:$str;
  $unreasonableage:$str
};

f=parseage(s:$str):i64!$parseerr{
  let n=str.toint(s)!$parseerr;
  if(n<0){
    <$parseerr{$negativeage:s};
  };
  if(n>150){
    <$parseerr{$unreasonableage:s};
  };
  <n;
};
```

## Error propagation with `!`

The `!` operator is the primary way to handle errors from callees. It propagates errors upward automatically:

```text
f=getuser(id:u64):$user!$apierr{
  let row=db.one("SELECT * FROM users WHERE id=?";@(id))!$apierr;
  <$user{id:row.u64("id");name:row.str("name")};
};
```

Here is what `!$apierr` does:

1. Call `db.one(...)` which returns a `$row!$dberr` union
2. If the result is `$ok(row)`: unwrap and continue, `row` gets the value
3. If the result is `$err(e)`: return the error from `getuser`

The `!` operator makes error propagation concise. Without it, you would need a match expression to handle each fallible call. With `!`, one line handles the unwrap-or-propagate pattern.

### Chaining propagation

Multiple fallible calls can be chained, each with their own error mapping:

```text
f=handle(req:http.$req):http.$res!$apierr{
  let body=json.dec(req.body)!$apierr;
  let user=db.getuser(body.id)!$apierr;
  let updated=db.save(user)!$apierr;
  <http.$res.ok(json.enc(updated));
};
```

Each `!` is a potential early return. If any call fails, the function returns immediately with the mapped error. If all succeed, execution reaches the final return.

## Matching on errors for recovery

When you need to handle errors instead of propagating them, use a match expression:

```text
f=getordefault(id:u64):$user{
  <mt db.getuser(id) {
    $ok:user  user;
    $err:e    $user{id:0;name:"anonymous"}
  };
};
```

This function is total (no `!` in its return type) because it handles all errors internally.

### Matching specific error variants

You can match on the error type's variants to handle different failures differently:

```text
f=resilientget(id:u64):http.$res{
  <mt db.getuser(id) {
    $ok:user  http.$res.ok(json.enc(user));
    $err:e    mt e {
      $notfound:x          http.$res.status(404;"User not found");
      $timeout:x           http.$res.status(503;"Service temporarily unavailable");
      $connectionfailed:x  http.$res.status(503;"Connection failed");
      $queryfailed:msg     http.$res.status(500;msg)
    }
  };
};
```

## A complete error handling example

Here is a full program demonstrating the error model:

```
m=calc;
i=io:std.io;

t=$calcerr{
  $divbyzero:bool;
  $invalidop:$str
};

f=divide(a:f64;b:f64):f64!$calcerr{
  if(b=0.0){
    <$calcerr{$divbyzero:true};
  };
  <a/b;
};

f=calc(a:f64;op:$str;b:f64):f64!$calcerr{
  if(op="+"){<a+b};
  if(op="-"){<a-b};
  if(op="*"){<a*b};
  if(op="/"){
    let result=divide(a;b)!$calcerr;
    <result;
  };
  <$calcerr{$invalidop:op};
};

f=main():i64{
  mt calc(10.0;"/";3.0) {
    $ok:v   io.println(v as $str);
    $err:e  mt e {
      $divbyzero:x  io.println("Error: division by zero");
      $invalidop:op io.println("Error: unknown operator")
    }
  };
  <0;
};
```

## Exercises

### Exercise 1: Safe division

Write a function `f=safediv(a:i64;b:i64):i64!$matherr` that returns a `$divbyzero` error when `b` is zero. Write a `main` function that calls it and prints either the result or an error message using match.

### Exercise 2: Lookup with error

Define an error type `t=$lookuperr{$notfound:$str;$emptymap:bool}`. Write a function `f=lookup(m:@($str:i64);key:$str):i64!$lookuperr` that:
- Returns `$emptymap` if the map has zero entries
- Returns `$notfound(key)` if the key does not exist
- Returns the value otherwise

### Exercise 3: Error chain

Write two functions:
- `f=parseint(s:$str):i64!$parseerr` that wraps `str.toint`
- `f=parseanddouble(s:$str):i64!$calcerr` that calls `parseint` and propagates the error as `$calcerr`, then doubles the result

This exercises the `!` propagation with error type mapping.

## Key takeaways

- toke has no exceptions -- errors are values in the type system
- Error types are sum types: `t=$myerr{$variant1:$type1;$variant2:$type2}`
- `:T!E` in a return type means the function can return either `T` (success) or `E` (error)
- `expr!$errtype` propagates errors upward to the current function's error type
- `mt expr {$ok:v handlesuccess; $err:e handleerror}` matches on results for recovery
- Match is exhaustive -- every variant must be handled
- Functions without `!` are total and cannot propagate errors

## Next

[Lesson 6: Strings and I/O](/docs/learn/06-strings-io/) -- string operations, file I/O, and JSON.
