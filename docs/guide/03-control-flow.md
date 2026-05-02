---
title: Lesson 3 — Control Flow
slug: 03-control-flow
section: learn
order: 3
---

**Estimated time: ~25 minutes**

## Conditionals with `if`

The `if` keyword evaluates a boolean condition and executes a block:

```
if(x>0){
  io.println("positive");
};
```

The condition must be of type `bool`. toke has no implicit truthiness -- you cannot write `if(x)` where `x` is an integer. You must write `if(!(x=0))`.

### If-else with `el`

The `el` keyword follows an `if` block to provide an alternative:

```
if(x>0){
  io.println("positive");
}el{
  io.println("not positive");
};
```

Note: `el` is not `else` -- it is two characters, consistent with toke's token-efficient design. The `el` block attaches directly to the closing `}` of the `if`.

### Chained conditions

There is no `elif` or `else if` keyword. Instead, nest an `if` inside the `el` block:

```
if(x>0){
  io.println("positive");
}el{
  if(x<0){
    io.println("negative");
  }el{
    io.println("zero");
  };
};
```

This is the only way to chain conditions. One form, no alternatives.

### Using `if` for early returns

A common pattern is to use `if` with `<` for guard clauses:

```
f=validate(age:i64):bool{
  if(age<0){<false};
  if(age>150){<false};
  <true;
};
```

## Loops with `lp`

toke has exactly one loop construct: `lp`. There is no `while`, `for`, `do-while`, or `for-each`.

```
lp(init;condition;step){
  body
};
```

The three parts are:

| Part | Purpose | Executed |
|------|---------|----------|
| `init` | Initialisation statement | Once, before the loop |
| `condition` | Boolean test | Before each iteration |
| `step` | Update statement | After each iteration body |

### Counting loop

```
m=count;
i=io:std.io;

f=main():i64{
  lp(let i=0;i<5;i=i+1){
    io.println("i = \(i as $str)");
  };
  <0;
};
```

This prints:
```
i = 0
i = 1
i = 2
i = 3
i = 4
```

### Sum of an array

```
f=sum(arr:@i64):i64{
  let acc=mut.0;
  lp(let i=0;i<arr.len;i=i+1){
    acc=acc+arr.get(i);
  };
  <acc;
};
```

The `arr.len` member gives the array length. Array elements are accessed with `arr.get(i)`.

### While-style loop

If you need a "while" loop, use `lp` with a no-op init and step:

```
let done=mut.false;
lp(let x=0;!done;x=0){
  if(somecondition){
    done=true;
  };
};
```

### Breaking out with `br`

The `br` keyword exits the innermost loop immediately:

```
f=findfirstneg(arr:@i64):i64{
  let result=mut.0-1;
  lp(let i=0;i<arr.len;i=i+1){
    if(arr.get(i)<0){
      result=arr.get(i);
      br;
    };
  };
  <result;
};
```

When `br` executes, control jumps past the closing `}` of the loop. There is no "break with value" -- assign to a mutable binding before breaking.

## Match expressions

The match expression destructures sum types (tagged unions). It uses the `mt` keyword:

```
mt result {
  $ok:val  val;
  $err:e   0
};
```

### How match works

1. The expression after `mt` is evaluated
2. The runtime checks which variant it is
3. The matching arm binds the payload and executes its expression

Each arm has the form:

```
$variantname:binding  result_expression;
```

### Match is exhaustive

The compiler requires every variant to be covered. If you add a variant to a sum type but forget to update a match expression, you get a compile error (E4010). This is a safety guarantee.

### Match on a custom sum type

```
m=shapes;

t=$shape{
  $circle:f64;
  $square:f64;
  $rect:f64
};

f=describe(s:$shape):$str{
  <mt s {$circle:r "circle";$square:side "square";$rect:w "rectangle"};
};
```

### Match on error results

Match is the primary way to handle errors when you cannot propagate them:

```text
f=safeget(arr:@i64;idx:u64):$str{
  <mt getelement(arr;idx) {
    $ok:val "Found";
    $err:e "Error: not found"
  };
};
```

We will cover error handling in depth in Lesson 5.

## Combining control flow

Here is a more complete example -- a function that classifies numbers in an array:

```
m=classify;
i=io:std.io;

f=classify(arr:@i64):void{
  let positives=mut.0;
  let negatives=mut.0;
  let zeroes=mut.0;

  lp(let i=0;i<arr.len;i=i+1){
    if(arr.get(i)>0){
      positives=positives+1;
    }el{
      if(arr.get(i)<0){
        negatives=negatives+1;
      }el{
        zeroes=zeroes+1;
      };
    };
  };

  io.println("Positives: \(positives as $str)");
  io.println("Negatives: \(negatives as $str)");
  io.println("Zeroes: \(zeroes as $str)");
};
```