---
title: Lesson 6 — Strings and I/O
slug: 06-strings-io
section: learn
order: 6
---

**Estimated time: ~20 minutes**

## The $str type

Strings in toke are UTF-8 encoded, heap-allocated values of type `$str`. String literals are delimited by double quotes:

```
let greeting="Hello, world!";
let empty="";
let escaped="Line one\nLine two";
```

### Escape sequences

| Sequence | Meaning |
|----------|---------|
| `\"` | Literal double quote |
| `\\` | Literal backslash |
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |
| `\0` | Null byte |
| `\xNN` | Byte with hex value NN |

### String interpolation

Insert expressions into strings with `\(expr)`:

```
let name="Alice";
let age=30;
let msg="Name: \(name), Age: \(age as $str)";
```

The expression inside `\(...)` must resolve to a `$str`-compatible type. Use `as $str` to convert numbers.

## String operations with std.str

Import the string module to access string functions:

```
i=str:std.str;
```

### Length

```
let s="Hello";
let n=str.len(s);
```

### Slicing

Extract a substring by start index and length:

```
let s="Hello, world!";
let hello=str.slice(s;0;5);
let world=str.slice(s;7;5);
```

### Search

```
let found=str.contains(s;"world");
let idx=str.indexof(s;"world");
```

### Replace

```
let result=str.replace("Hello, world!";"world";"toke");
```

### Split and join

```
let parts=str.split("a,b,c";",");
let joined=str.join(parts;"-");
```

### Case conversion

```
let upper=str.upper("hello");
let lower=str.lower("HELLO");
```

### Trim

```
let trimmed=str.trim("  hello  ");
```

## Console I/O with std.io

```
i=io:std.io;
```

### Printing

```
io.println("Hello, world!");
io.print("Enter your name: ");
```

### Reading input

```
let line=io.readline();
```

## File I/O with std.file

```
i=file:std.file;
```

### Reading a file

```
i=file:std.file;

t=$fileerr{
  $notfound:$str;
  $readfailed:$str
};

f=readconfig(path:$str):$str!$fileerr{
  let content=file.read(path)!$fileerr;
  <content;
};
```

`file.read` returns the entire file content as a `$str`. It is a fallible operation -- the file might not exist or might not be readable.

### Writing a file

```
i=file:std.file;

t=$fileerr{$notfound:$str;$readfailed:$str};

f=savedata(path:$str;data:$str):void!$fileerr{
  file.write(path;data)!$fileerr;
};
```

`file.write` creates the file if it does not exist, or overwrites it if it does.

### Appending to a file

```
i=file:std.file;

t=$fileerr{$notfound:$str;$readfailed:$str};

f=log(path:$str;msg:$str):void!$fileerr{
  file.append(path;msg)!$fileerr;
};
```

### Checking if a file exists

```
let exists=file.exists("config.json");
```

## JSON with std.json

```
i=json:std.json;
```

### Encoding (struct to JSON string)

```
i=json:std.json;

t=$user{id:u64;name:$str;email:$str};

f=usertojson(u:$user):$str{
  <json.enc(u);
};
```

### Decoding (JSON string to struct)

```
i=json:std.json;

t=$user{id:u64;name:$str;email:$str};
t=$jsonerr{
  $parsefailed:$str;
  $missingfield:$str
};

f=jsontouser(s:$str):$user!$jsonerr{
  let u=json.dec(s)!$jsonerr;
  <u;
};
```

### Working with dynamic JSON

For JSON whose structure you do not know at compile time, use `json.get` to extract fields:

```text
f=getname(raw:$str):$str!$jsonerr{
  let obj=json.dec(raw)!$jsonerr;
  let name=json.get(obj;"name")!$jsonerr;
  <name as $str;
};
```

## Practical example: word counter

Here is a complete program that reads a file and counts word frequencies:

```
m=wc;
i=io:std.io;
i=file:std.file;
i=str:std.str;

t=$wcerr{
  $fileerr:$str
};

f=countwords(text:$str):@($str:i64){
  let words=str.split(text;" ");
  let freq=mut.@();
  lp(let i=0;i<words.len;i=i+1){
    let w=str.lower(str.trim(words.get(i)));
    if(str.len(w)>0){
      if(freq.contains(w)){
        freq=freq.put(w;freq.get(w)+1);
      }el{
        freq=freq.put(w;1);
      };
    };
  };
  <freq;
};

f=printfreq(k:$str;v:i64):void{
  io.println(str.concat(str.concat(k;": ");v as $str));
};

f=run(content:$str):void{
  let freq=countwords(content);
  let keys=freq.keys;
  lp(let i=0;i<keys.len;i=i+1){
    let k=keys.get(i);
    printfreq(k;freq.get(k));
  };
};

f=main():i64{
  mt file.read("input.txt") {
    $ok:content  run(content);
    $err:e  io.println("Error reading file")
  };
  <0;
};
```