---
title: Statements
slug: statements
section: reference
order: 11
---

Statements are the building blocks of function bodies. They are separated by semicolons. The trailing semicolon before a closing `}` or at end-of-file may be omitted.

## `let` — Immutable Binding

Binds a name to a value. The binding is immutable: the name cannot be reassigned after it is created.

**Syntax:**
```
let name = expr;
let name: type = expr;
```

**Example:**

```toke
m=stmts;
f=demo():i64{
  let x=42;
  let y:i64=100;
  < x+y
};
```

The type annotation is optional when it can be inferred from the initializer. When both annotation and initializer are present, their types must match.

**Common mistake:** Attempting to reassign an immutable binding produces an error. Use `let x=mut.val` for mutable bindings.

---

## `let x=mut.val` — Mutable Binding

Binds a name to a value and marks it as mutable. The binding can be reassigned later with a plain assignment statement.

**Syntax:**
```
let name = mut.expr;
let name: type = mut.expr;
```

**Example:**

```toke
m=stmts;
f=demo():i64{
  let count=mut.0;
  count=count+1;
  count=count+1;
  < count
};
```

Result: `2`

The `mut.` prefix is part of the initializer, not a type modifier. It tells the compiler that this binding participates in the mutable-variable tracking.

---

## Reassignment

Assigns a new value to a mutable binding. The name must have been introduced with `let name=mut.val`.

**Syntax:**
```
name = expr;
```

**Example:**

```toke
m=stmts;
f=demo():$str{
  let msg=mut."initial";
  msg="updated";
  < msg
};
```

**Common mistake:** Attempting to reassign an immutable `let` binding is a compile-time error. Always use `let x=mut.val` when reassignment is needed.

---

## `<` — Return

The `<` operator returns a value from the current function. There is no `return` keyword.

**Syntax:**
```
< expr
```

**Example:**

```toke
m=stmts;
f=abs(n:i64):i64{
  if(n<0){< 0-n}el{< n}
};
```

`<` can appear anywhere in a function body, including inside `if` branches. The type of `expr` must match the function's declared return type.

**Common mistake:** Using `return` — it is not a toke keyword. Use `<` instead.

---

## `if` / `el` — Conditional

Executes one of two blocks depending on a condition. The condition must be of type `bool`. The `el` clause is optional.

**Syntax:**
```
if(cond){body}
if(cond){body}el{body}
```

**Example — with else:**

```toke
m=stmts;
f=grade(score:i64):$str{
  if(!(score<90)){< "A"}el{
    if(!(score<75)){< "B"}el{< "C"}
  }
};
```

**Example — without else:**

```toke
m=stmts;
f=logactive(active:bool):void{
  if(active){
    let msg="active";
    let dummy=msg
  }
};
```

**Common mistakes:**
- Using `else` — toke uses `el`
- Using a non-`bool` condition — the compiler rejects this

---

## `lp` — Loop

The `lp` statement is a C-style for loop with an optional initialiser, condition, and step.

**Syntax:**
```
lp(init; cond; step){ body }
lp(;;){ body }
```

All three parts are optional. Omitting the condition creates an infinite loop. The `init` part supports `let name=expr` (immutable) only — not `mut`.

**Example — counted loop:**

```toke
m=stmts;
f=sumto(n:i64):i64{
  let acc=mut.0;
  lp(let i=0;i<n;i=i+1){
    acc=acc+i
  };
  < acc
};
```

**Example — condition-only loop (while-style):**

```toke
m=stmts;
f=countup(limit:i64):i64{
  let n=mut.0;
  lp(let i=0;i<limit;i=i+1){
    n=n+1
  };
  < n
};
```

**Notes:**
- The loop variable declared in `init` is scoped to the loop
- The step expression runs after each iteration of the body
- There is no `break` or `continue` — loops run to completion unless the condition becomes false

---

## Match — `mt expr {$variant:v body; ...}`

The `mt` keyword introduces a match expression on a sum type or error union value. Each arm binds the inner value to a name and provides a body expression.

**Syntax:**
```
mt expr {
  Variant1:name body;
  Variant2:name body
}
```

Arms are separated by `;`. The trailing `;` is optional. All arms must produce the same type.

**Example — sum type match:**

```toke
m=stmts;
t=$direction{$north:void;$south:void;$east:void;$west:void};
f=opposite(d:$direction):$direction{
  < mt d {
    $north:v $direction{$south:0};
    $south:v $direction{$north:0};
    $east:v $direction{$west:0};
    $west:v $direction{$east:0}
  }
};
```

**Example — error union match:**

```toke
m=stmts;
i=file:std.file;
f=readornull(path:$str):$str{
  let r=file.read(path);
  < mt r {
    $ok:data data;
    $err:e ""
  }
};
```

**Common mistakes:**
- Non-exhaustive match — every variant must be covered, or the compiler emits [E4010](/docs/reference/errors/#e4010)
- Inconsistent arm types — all arms must produce the same type ([E4011](/docs/reference/errors/#e4011))

---

## Arena Block — `{arena ...}`

An arena block creates a scoped memory region. All allocations made inside the block are freed when the block exits.

**Syntax:**
```
{arena
  stmts
}
```

**Example:**

```toke
m=stmts;
f=demo():i64{
  let result=mut.0;
  {arena
    let tmp=@(1;2;3);
    result=tmp.len as i64
  }
  < result
};
```

**Constraint:** A value allocated inside an arena block cannot be assigned to a variable declared in an outer scope. Doing so produces error [E5001](/docs/reference/errors/#e5001). Arena-scoped values may be used freely within the block itself, and their computed results (non-pointer values) may be written to outer mutable bindings.
