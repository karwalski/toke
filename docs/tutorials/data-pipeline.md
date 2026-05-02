---
title: "Tutorial: Data Pipeline"
slug: data-pipeline
section: tutorials
order: 6
---

# tutorial: data pipeline

build a csv data processing pipeline that reads a csv file, computes
column statistics (sum, average, min, max), and writes a json report.

## what we're building

the finished program takes two arguments -- an input csv path and an output
json path. it detects which columns are numeric, calculates descriptive
statistics for each, prints a summary table to stdout, and writes the full
report as json.

```
$ ./datapipe sales.csv report.json
reading: sales.csv
file:    sales.csv
rows:    5
columns: 5

numeric column statistics:
  column          count       sum       avg       min       max
  --------------- -----  --------  --------  --------  --------
  price  5  257.98  51.60  29.99  99.00
  quantity  5  58  11.60  3  25
  discount  5  0.40  0.08  0.00  0.15

report written to: report.json
```

**source code:** `toke/examples/datapipe/`

## llm prompts

generate the skeleton with these three prompts, each building on the
previous output.

**prompt 1 -- project scaffold and types**

> write a toke program in 55-char default mode. define a module
> `datapipe.main`. import `std.io`, `std.file`, `std.csv`, `std.json`,
> `std.str`, `std.math`, `std.args`. define three types: `$pipeerr` -- a
> sum type with variants `$fileerr:$str`, `$csverr:$str`, `$usage:$str`;
> `$colstats` -- a product type with fields `name:$str`, `count:u64`,
> `sum:f64`, `avg:f64`, `min:f64`, `max:f64`; and `$report` -- a product
> type with `filename:$str`, `rows:u64`, `columns:u64`, `stats:@$colstats`.

**prompt 2 -- csv reading and numeric detection**

> add a function `readcsv(path:$str):csv.$doc!$pipeerr` that reads a
> file and parses it as csv, returning `$pipeerr.$fileerr` or
> `$pipeerr.$csverr` on failure. add `isnumeric(s:$str):bool` that
> tries `str.tofloat` and returns true/false. add
> `detectnumericcols(doc:csv.$doc):@bool` that loops every column,
> checks every non-empty cell with `isnumeric`, and returns an array of
> booleans.

**prompt 3 -- statistics and json output**

> add `computecolstats(doc:csv.$doc;col:i64;name:$str):$colstats`
> that loops rows, accumulates count/sum/min/max, and computes avg. add
> `computeallstats(doc:csv.$doc):@$colstats` that calls it for each
> numeric column. add `reporttojson(r:$report):$str` that builds the
> json string manually with `str.buf`/`str.add`/`str.done`. add
> `printsummary(r:$report):void` that prints a formatted table to
> stdout. wire everything in a `run():i64` function and call it from
> `main`.

## step by step

### 1. create the project

```bash
mkdir datapipe && cd datapipe
touch main.tk
```

### 2. module declaration and imports

```toke
m=datapipe.main;
i=io:std.io;
i=file:std.file;
i=csv:std.csv;
i=json:std.json;
i=str:std.str;
i=math:std.math;
i=args:std.args;
```

`m=` declares the module path. each `i=` binds a standard library module
to a local alias.

### 3. define types

```toke
t=$pipeerr{$fileerr:$str;$csverr:$str;$usage:$str};
t=$colstats{name:$str;count:u64;sum:f64;avg:f64;min:f64;max:f64};
t=$report{filename:$str;rows:u64;columns:u64;stats:@$colstats};
```

`$pipeerr` is a sum type (tagged union) with three error variants.
`$colstats` and `$report` are product types (structs). `@$colstats`
means "array of `$colstats`".

### 4. parse arguments and read csv

```toke
f=parseargs():@$str!$pipeerr{
  let argc=args.count();
  if(argc<3){
    <$err($pipeerr.$usage("usage: datapipe <input.csv> <output.json>"))
  };
  let input=args.get(1);
  let output=args.get(2);
  <$ok(@(input;output))
};

f=readcsv(path:$str):csv.$doc!$pipeerr{
  let raw=mt file.read(path) {
    $ok:v v;
    $err:e <$err($pipeerr.$fileerr(
      str.concat("cannot read file: ";path)
    ))
  };
  let doc=mt csv.parse(raw) {
    $ok:d d;
    $err:e <$err($pipeerr.$csverr("failed to parse csv"))
  };
  <$ok(doc)
};
```

the return type `@$str!$pipeerr` means "array of strings or a `$pipeerr`
error". `<` is return. `@(input;output)` constructs a two-element array.
the `mt` keyword introduces pattern matching on a result -- `$ok:v v` means
"if ok, bind to `v` and evaluate to `v`".

### 5. detect numeric columns

```toke
f=isnumeric(s:$str):bool{
  let v=mt str.tofloat(str.trim(s)) {
    $ok:v true;
    $err:e false
  };
  <v
};

f=detectnumericcols(doc:csv.$doc):@bool{
  let headers=csv.headers(doc);
  let ncols=headers.len;
  let result=mut.@();

  lp(let c=0;c<(ncols as i64);c=c+1){
    let numeric=mut.true;
    let nrows=csv.rowcount(doc);
    let checked=mut.0;
    lp(let r=0;r<(nrows as i64);r=r+1){
      let val=csv.cell(doc;r;c);
      if(str.len(str.trim(val))>0){
        if(isnumeric(val)==false){numeric=false};
        checked=checked+1
      }
    };
    if(checked==0){numeric=false};
    result=result.push(numeric)
  };
  <result
};
```

`mut.@()` creates a mutable empty array. `lp(init;cond;step){body}` is
a c-style loop. `csv.cell(doc;r;c)` fetches row `r`, column `c` --
semicolons separate arguments. empty cells are skipped; a column with
zero non-empty cells is marked non-numeric.

### 6. compute statistics

```toke
f=computecolstats(doc:csv.$doc;col:i64;name:$str):$colstats{
  let nrows=csv.rowcount(doc);
  let count=mut.0;
  let sum=mut.0.0;
  let mn=mut.999999999.0;
  let mx=mut.(0.0-999999999.0);

  lp(let r=0;r<(nrows as i64);r=r+1){
    let val=csv.cell(doc;r;col);
    if(str.len(str.trim(val))>0){
      let num=mt str.tofloat(str.trim(val)) {
        $ok:v v;
        $err:e 0.0
      };
      count=count+1;
      sum=sum+num;
      if(num<mn){mn=num};
      if(num>mx){mx=num}
    }
  };
  let avg=0.0;
  if(count>0){avg=sum/((count) as f64)};
  <$colstats{
    name:name;count:(count as u64);sum:sum;
    avg:avg;min:mn;max:mx
  }
};

f=computeallstats(doc:csv.$doc):@$colstats{
  let headers=csv.headers(doc);
  let numeric=detectnumericcols(doc);
  let result=mut.@();
  lp(let c=0;c<(headers.len as i64);c=c+1){
    if(numeric.get(c)){
      let stats=computecolstats(doc;c;headers.get(c));
      result=result.push(stats)
    }
  };
  <result
};
```

`(count as u64)` and `(count) as f64` -- toke requires explicit casts
between integer and float types. `mut.(0.0-999999999.0)` initialises a
mutable to a negative float (no negative literal; subtract from zero).

### 7. serialise to json

toke builds json strings with a string buffer -- `str.buf()` allocates,
`str.add()` appends, `str.done()` finalises. two functions handle this:
`colstatstojson` serialises one column's stats as a json object, and
`reporttojson` wraps all columns into the top-level report structure.
see the full source for the complete implementations -- the pattern is
always `str.add(buf;"key:");str.add(buf;value)` repeated for each field.

### 8. print summary and wire together

`printsummary` formats a table to stdout using `str.buf` the same way
as the json serialiser. the `run` function orchestrates the full
pipeline:

```toke
f=run():i64{
  let paths=mt parseargs() {
    $ok:v v;
    $err:e {
      mt e {$usage:msg io.eprintln(msg);
         $fileerr:msg io.eprintln(msg);
         $csverr:msg io.eprintln(msg)};
      <1
    }
  };
  let input=paths.get(0);
  let output=paths.get(1);
  io.println(str.concat("reading: ";input));
  let doc=mt readcsv(input) {
    $ok:d d;
    $err:e {
      mt e {$fileerr:msg io.eprintln(msg);
         $csverr:msg io.eprintln(msg);
         $usage:msg io.eprintln(msg)};
      <1
    }
  };
  let stats=computeallstats(doc);
  let r=$report{
    filename:input;
    rows:(csv.rowcount(doc) as u64);
    columns:(csv.headers(doc).len as u64);
    stats:stats
  };
  printsummary(r);
  io.println("");
  let jsonstr=reporttojson(r);
  mt file.write(output;jsonstr) {
    $ok:v {io.println(str.concat("report written to: ";output))};
    $err:e {io.eprintln(str.concat("error writing report: ";output));<1}
  };
  <0
};

f=main():i64{<run()};
```

the nested `mt` shows a key toke pattern: the outer match unwraps
`$ok`/`$err`, and the inner match dispatches the error variant so each
gets a meaningful message on stderr.

## build and run

compile the program:

```bash
tkc main.tk -o datapipe
```

create a sample csv file called `sales.csv`:

```csv
product,region,price,quantity,discount
widget,north,29.99,10,0.05
gadget,south,49.50,25,0.10
widget,east,29.99,8,0.00
gizmo,west,99.00,3,0.15
gadget,north,49.50,12,0.10
```

run the pipeline:

```bash
$ ./datapipe sales.csv report.json
```