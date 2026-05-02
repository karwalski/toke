---
title: Cookbook — Document pipeline
slug: doc-pipeline
section: reference/cookbook
order: 2
---

A batch processing pipeline that walks a directory of text files, counts words and lines in each, and logs a structured summary. The program shows how to compose `std.file`, `std.str`, and `std.log` into a straightforward processing loop without any external dependencies.

## What this demonstrates

- Listing directory contents with `file.list`
- Reading each file with `file.read`
- Splitting and measuring strings with `str.split`, `str.len`
- Building summary strings with `str.concat` and `str.fromInt`
- Emitting structured log output with `log.info` and `log.warn`
- A `lp` loop over an array with `.len` and `.get(i)`

## The program

```toke
m=docpipeline;

i=file:std.file;
i=str:std.str;
i=log:std.log;

t=$err{code:i64};

f=countlines(content:$str):i64{
  let lines=str.split(content; "\n");
  < lines.len as i64
};

f=countwords(content:$str):i64{
  let words=str.split(content; " ");
  < words.len as i64
};

f=summarise(name:$str;lines:i64;words:i64):$str{
  let lstr=str.fromInt(lines);
  let wstr=str.fromInt(words);
  < str.concat(name;
       str.concat(" lines=";
         str.concat(lstr;
           str.concat(" words="; wstr))))
};

f=processfile(name:$str;content:$str):i64{
  let lines=countlines(content);
  let words=countwords(content);
  let summary=summarise(name; lines; words);
  log.info(summary);
  < 1
};

f=skipfile(name:$str):i64{
  log.warn(str.concat("skipped: "; name));
  < 0
};

f=processdir(dir:$str):i64!$err{
  let entries=file.list(dir)!$err;
  let total=mut.0;
  lp(let i=0;i<entries.len as i64;i=i+1){
    let name=entries.get(i);
    let fullpath=str.concat(dir; str.concat("/"; name));
    let counted=file.read(fullpath)|{
      $ok:content processfile(name; content);
      $err:e skipfile(name)
    };
    total=total+counted
  };
  log.info(str.concat("processed "; str.fromInt(total)));
  < 0
};

f=main():i64!$err{
  let dir="docs";
  log.info(str.concat("scanning: "; dir));
  processdir(dir)!$err;
  < 0
};
```

## Walking through the code

**`f=countlines`** — Splits the file content on newline characters and returns the array length as `i64`. `str.split` returns an `@($str)` array; `.len` gives the element count as `u64`, cast to `i64` for arithmetic.

**`f=countwords`** — Same approach with a space delimiter. This is a simple whitespace split, adequate for plain prose files.

**`f=summarise`** — Assembles the log line for one file. `str.fromInt` converts the integer counts to strings, then a chain of `str.concat` calls builds the final message. toke has no string interpolation, so nesting `str.concat` is the idiomatic approach.

**`f=processfile`** — Called in the `Ok` match arm when a file is successfully read. Computes and logs the word and line counts, then returns `1` so the caller can accumulate a total.

**`f=skipfile`** — Called in the `Err` match arm when a file cannot be read. Emits a warning and returns `0` so the total is not incremented.

**`f=processdir`** — The main loop body:

1. `file.list(dir)` returns an array of entry names in the directory. The `!$err` propagates any failure immediately.
2. `total` is declared mutable with `let total=mut.0;` so it can be incremented inside the loop.
3. The `lp` loop iterates from `i=0` while `i<entries.len as i64`, stepping `i=i+1`.
4. Each iteration retrieves the entry name with `.get(i)`, builds the full path, and calls `file.read`.
5. The result is matched with single-expression arms: `processfile` on `Ok` and `skipfile` on `Err`. Each returns `1` or `0` which is added to `total`.
6. After the loop the total processed count is logged.

**`f=main`** — Passes the `"docs"` directory to `processdir`. Change this string to point at any directory of text files.

## Running the program

```shell
tkc docpipeline.tk
./docpipeline
```

Sample output:

```
[INFO] scanning: docs
[INFO] readme.txt lines=42 words=310
[INFO] changelog.txt lines=18 words=95
[WARN] skipped: .DS_Store
[INFO] processed 2
```

## Key syntax reminders

| Pattern | Syntax |
|---------|--------|
| Mutable variable | `let x=mut.0;` then `x=x+1` |
| Loop | `lp(let i=0; i<n; i=i+1){body}` |
| Array length | `arr.len` |
| Array subscript | `arr.get(i)` |
| Int to string | `str.fromInt(n)` |
| Split string | `str.split(s; delim)` |
