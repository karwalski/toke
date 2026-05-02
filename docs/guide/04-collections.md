---
title: Lesson 4 — Collections
slug: 04-collections
section: learn
order: 4
---

**Estimated time: ~25 minutes**

## Arrays

An array is an ordered, dynamically-sized sequence of elements of the same type.

### Array literals

Array literals use `@(...)` with semicolons separating elements:

```
let nums=@(1;2;3;4;5);
let names=@("Alice";"Bob";"Charlie");
let empty=@();
```

Note the last line -- an empty array literal `@()` has element type `unknown` until constrained by context such as a type annotation or assignment.

### Array types

The type of an array is written `@T` where `T` is the element type:

The parameter type `@$str` denotes an array of strings. The return type `@i64` denotes an array of integers.

```
f=process(items:@$str):void{
};

f=makenumbers():@i64{
  <@(10;20;30);
};
```

### Array length

Every array has a `.len` member that returns the number of elements as `u64`:

```
let arr=@(1;2;3);
let size=arr.len;
```

### Indexing

Access elements by constant index using dot notation, or by variable using `.get(i)`. Indices are zero-based:

```
let arr=@(10;20;30);
let first=arr.get(0);
let second=arr.get(1);
let third=arr.get(2);
```

Out-of-bounds access is a runtime trap (RT001) -- the program terminates with a structured error. There is no silent undefined behaviour.

### Iterating over arrays

Use `lp` with an index variable:

```
i=io:std.io;

f=printall(arr:@$str):void{
  lp(let i=0;i<arr.len;i=i+1){
    io.println(arr.get(i));
  };
};
```

This is the standard iteration pattern in toke. There is no `for-each` or iterator protocol in v0.1.

### Building arrays

To build an array dynamically, start with an empty array and use append operations:

```
f=range(n:i64):@i64{
  let result=mut.@();
  lp(let i=0;i<n;i=i+1){
    result=result.push(i);
  };
  <result;
};
```

## Maps

A map is an unordered collection of key-value pairs.

### Map literals

Map literals use `@(key:value;...)` with `key:value` pairs separated by semicolons. The `@()` syntax is shared with arrays -- the parser distinguishes them by the `:` between key and value:

```
let ages=@("Alice":30;"Bob":25;"Charlie":35);
let config=@("host":"localhost";"port":"8080");
let empty=@();
```

### Map types

The type of a map is written `@(K:V)` where `K` is the key type and `V` is the value type:

```
f=process(lookup:@($str:i64)):void{
};
```

### Map operations

Maps support these core operations:

#### Get a value

```
let ages=@("Alice":30;"Bob":25);
let age=ages.get("Alice");
```

Accessing a key that does not exist is a runtime trap. Use `.contains` to check first.

#### Check if a key exists

```
if(ages.contains("Alice")){
  let age=ages.get("Alice");
  io.println("Alice is \(age as $str)");
};
```

#### Put a key-value pair

```
let ages=mut.@("Alice":30;"Bob":25);
ages=ages.put("Charlie";35);
```

#### Delete a key

```
ages=ages.delete("Bob");
```

#### Get the size

```
let count=ages.len;
```

### Iterating over maps

To iterate over a map, retrieve its keys and iterate over that array:

```
i=io:std.io;
i=str:std.str;

f=printmap(mp:@($str:i64)):void{
  let keys=mp.keys;
  lp(let i=0;i<keys.len;i=i+1){
    let k=keys.get(i);
    let v=mp.get(k);
    io.println(str.concat(str.concat(k;": ");v as $str));
  };
};
```

## Practical patterns

### Frequency counter

Count how many times each word appears:

```
m=freq;
i=io:std.io;

f=countfreq(words:@$str):@($str:i64){
  let freq=mut.@();
  lp(let i=0;i<words.len;i=i+1){
    let w=words.get(i);
    if(freq.contains(w)){
      let cur=freq.get(w);
      freq=freq.put(w;cur+1);
    }el{
      freq=freq.put(w;1);
    };
  };
  <freq;
};
```

### Finding a value in an array

```
f=contains(arr:@i64;target:i64):bool{
  lp(let i=0;i<arr.len;i=i+1){
    if(arr.get(i)=target){
      <true;
    };
  };
  <false;
};
```

Note: equality comparison in toke uses `=` (single equals) in expression context. The parser distinguishes assignment (statement position) from comparison (expression position) by context.

### Array reversal

```
f=reverse(arr:@i64):@i64{
  let result=mut.@();
  let i=mut.arr.len;
  lp(let x=0;i>0;x=0){
    i=i-1;
    result=result.push(arr.get(i));
  };
  <result;
};
```

## Exercises

### Exercise 1: Sum and average

Write two functions:
- `f=sum(arr:@i64):i64` -- returns the sum of all elements
- `f=average(arr:@i64):f64` -- returns the average as a float (use `as f64` to cast the sum and length)

### Exercise 2: Frequency counter

Write a complete program with module `freq` that:
1. Takes an array of strings `@("apple";"banana";"apple";"cherry";"banana";"apple")`
2. Counts the frequency of each word using a map
3. Prints each word and its count

### Exercise 3: Array reversal

Write `f=reverse(arr:@$str):@$str` that returns a new array with elements in reverse order. Test it with `@("a";"b";"c";"d")`.

### Exercise 4: Merge maps

Write `f=merge(a:@($str:i64);b:@($str:i64)):@($str:i64)` that returns a new map containing all keys from both maps. If a key exists in both, use the value from `b`.

## Key takeaways

- Array literals: `@(1;2;3)`, type: `@i64`
- Map literals: `@("a":1;"b":2)`, type: `@($str:i64)`
- `.len` gives the size of arrays and maps
- Array indexing: `arr.get(i)` -- zero-based, bounds-checked
- Map operations: `.get`, `.put`, `.delete`, `.contains`, `.keys`
- Iterate with `lp` and an index variable
- Out-of-bounds access is a runtime trap, not undefined behaviour

## Next

[Lesson 5: Error Handling](/docs/learn/05-errors/) -- error types, propagation, and recovery.
