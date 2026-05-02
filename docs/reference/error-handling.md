---
title: Error Handling
slug: error-handling
section: reference
order: 2
---

toke makes errors explicit via the type system. A function that can fail returns an error union type. The caller must handle or propagate the error — there are no exceptions and no implicit error propagation.

## Error Union Type — `T!$errtype`

An error union type represents either a success value of type `T` or an error. The syntax is `T!$errtype` where `$errtype` is a named error type (conventionally `$err`).

```toke
m=test;
t=$err{msg:$str};
f=divide(a:i64;b:i64):i64!$err{
  if(b=0){
    < $err{msg:"division by zero"}
  }el{
    < a/b
  }
};
f=main():i64{<0;};
```

The success path returns a value of type `T`. The error path returns an error value.

### Common error types

The stdlib defines `$err` as the general-purpose error type used by most standard library functions. Individual modules may define more specific error types.

---

## Error Return

To return an error from a function, use `<` with an error value. The function's return type must be an error union.

```toke
m=errs;
i=file:std.file;
t=$err{msg:$str};
f=checkexists(path:$str):bool!$err{
  let stat=file.stat(path);
  < mt stat {
    $ok:info true;
    $err:e $err{msg:"file not found"}
  }
};
```

The `<` operator works the same way for error values as for success values — it is still the return operator.

---

## Error Match — `mt result {$ok:v ...; $err:e ...}`

To handle an error union value, use the `mt` match expression. This binds the success value or error value to a local name and provides a body for each case.

**Syntax:**
```
mt result {
  $ok:name success-body;
  $err:name error-body
}
```

**Example:**

```toke
m=errs;
i=file:std.file;
f=readsize(path:$str):i64{
  let result=file.read(path);
  < mt result {
    $ok:data data.len as i64;
    $err:e 0
  }
};
```

Both arms must produce the same type. The `$ok:` arm binds the inner success value; the `$err:` arm binds the error value. Either arm can use `<` to return early from the enclosing function.

**Example with early return on error:**

```toke
m=errs;
i=file:std.file;
t=$err{msg:$str};
f=firstline(path:$str):$str!$err{
  let data=file.read(path)!$err;
  < data
};
```

---

## Error Propagation — `expr!$errtype`

The `!` postfix operator is the propagation shorthand. It unwraps an error union: if the value is `$ok`, the inner value is produced; if it is `$err`, the error is immediately returned from the enclosing function.

**Syntax:**
```
expr!$errtype
```

**Example:**

```toke
m=errs;
i=file:std.file;
t=$err{msg:$str};
f=linecount(path:$str):i64!$err{
  let data=file.read(path)!$err;
  < data.len as i64
};
```

Here `file.read(path)!$err` means: if `file.read` returns an `$err`, propagate it immediately (the function returns that error); otherwise bind the file content to `data` and continue.

**Constraint:** The `!` operator can only be used inside a function whose return type is also an error union. Using it elsewhere produces error [E3020](/docs/reference/errors/#e3020).

---

## Complete Example 1 — File Read with Error Handling

```toke
m=errs;
i=file:std.file;
i=log:std.log;
t=$err{msg:$str};

f=processfile(path:$str):i64!$err{
  let data=file.read(path)!$err;
  let size=data.len as i64;
  if(size=0){
    < $err{msg:"empty file"}
  }el{
    < size
  }
};

f=main():void{
  let result=processfile("input.txt");
  mt result {
    $ok:n log.info("size: ");
    $err:e log.error("failed")
  }
};
```

This example shows:
- `!$err` propagation in `processfile`
- Explicit error return with `< $err{...}`
- Match in `main` to handle both outcomes without propagating further

---

## Complete Example 2 — HTTP Request with Fallback

```toke
m=errs;
i=http:std.http;
i=log:std.log;

f=fetch(url:$str):$str{
  let result=http.get(url);
  < mt result {
    $ok:resp resp;
    $err:e ""
  }
};

f=main():void{
  let body=fetch("https://api.example.com/data");
  if(!(body="")){ log.info("got data") }el{ log.warn("no data") }
};
```

This example shows:
- Absorbing an error at the call site (converting to a default value)
- Using `!(body="")` for "not equal" comparison
- No error union in `main` — errors are fully handled in `fetch`

---

## Summary

| Construct | Syntax | Purpose |
|-----------|--------|---------|
| Error union type | `T!$errtype` | Function return type that may fail |
| Error return | `< $err{...}` | Return an error value |
| Error match | `mt r {$ok:v ...;$err:e ...}` | Handle both outcomes explicitly |
| Propagation | `expr!$errtype` | Propagate error or unwrap success |
