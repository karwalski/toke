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
file:    sales.csv
rows:    5
columns: 5

numeric column statistics:
  price (count 5)
    sum=257.98
    avg=51.60
    min=29.99
    max=99.00
  quantity (count 5)
    sum=58.00
    avg=11.60
    min=3.00
    max=25.00
  discount (count 5)
    sum=0.40
    avg=0.08
    min=0.00
    max=0.15

report written to: report.json
```

and `report.json` contains:

```json
{"filename":"sales.csv","rows":5,"columns":5,"stats":[{"name":"price","count":5,"sum":257.98,"min":29.99,"max":99.00,"avg":51.60},{"name":"quantity","count":5,"sum":58.00,"min":3.00,"max":25.00,"avg":11.60},{"name":"discount","count":5,"sum":0.40,"min":0.00,"max":0.15,"avg":0.08}]}
```

**source code:** `toke/examples/datapipe/`

## llm prompts

generate the skeleton with these three prompts, each building on the
previous output.

**prompt 1 -- project scaffold and types**

> write a toke program in 55-char default mode. define a module
> `datapipe.main`. import `std.io`, `std.file`, `std.csv`, `std.str`,
> `std.args`. define two types: `$colstats` -- a product type with fields
> `name:$str`, `count:u64`, `sum:f64`, `min:f64`, `max:f64`, `avg:f64`; and
> `$report` -- a product type with `filename:$str`, `rows:u64`,
> `columns:u64`, `stats:@$colstats`.

**prompt 2 -- csv reading and numeric detection**

> `csv.parse(bytes)` returns an array of `$csvrow` (each has a `.fields`
> array); row 0 is the header. add a `cell(rows;r;c)` helper that returns the
> value at data row `r`, column `c`. add `isnumeric(s:$str):bool` that tries
> `str.tofloat` and returns true/false. add
> `iscolnumeric(rows;c;ndata):bool` that returns true only if a column has at
> least one value and every non-empty cell parses as a number.

**prompt 3 -- statistics and json output**

> add `computecol(rows;c;ndata;name):$colstats` that loops the data rows,
> accumulates count/sum/min/max, and computes avg. add
> `colstatstojson(s:$colstats):$str` and `reporttojson(r:$report):$str` that
> build the json string manually with `str.buf`/`str.add`/`str.done`. add
> `printstats(s:$colstats):$i64` that prints one column to stdout. wire
> everything in `main`: read the input csv, compute stats for each numeric
> column, print the summary, and write the json report to the output path.

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
i=str:std.str;
i=args:std.args;
```

`m=` declares the module path. Each `i=` binds a standard library module to a
local alias. We read the file with `std.file`, parse it with `std.csv`, and
take CLI arguments from `std.args`.

### 3. define types

```toke
t=$colstats{name:$str;count:u64;sum:f64;min:f64;max:f64;avg:f64};

t=$report{filename:$str;rows:u64;columns:u64;stats:@$colstats};
```

`$colstats` holds the summary for one numeric column. `$report` is the
top-level structure written out as JSON. `@$colstats` means "array of
`$colstats`".

### 4. read the CSV

`csv.parse` takes the file's bytes and returns an array of `$csvrow` (each row
has a `.fields` array). Row 0 is the header; data rows start at index 1. A
small `cell` helper hides that offset:

```toke
f=cell(rows:@$csvrow;r:i64;c:i64):$str{
  let row=rows.get(r+1);
  <row.fields.get(c)
};
```

The file read and CSV parse happen in `main` (below) using the result-matching
pattern `mt csv.parse(...) {$ok:v v; $err:e @()}`.

### 5. detect numeric columns

```toke
f=isnumeric(s:$str):bool{
  let t=str.trim(s);
  if(str.len(t)==0){<false};
  <mt str.tofloat(t) {
    $ok:x true;
    $err:e false
  }
};

f=iscolnumeric(rows:@$csvrow;c:i64;ndata:i64):bool{
  let allnum=mut.true;
  let any=mut.false;
  lp(let r=0;r<ndata;r=r+1){
    let v=str.trim(cell(rows;r;c));
    if(str.len(v)>0){
      any=true;
      if(isnumeric(v)==false){
        allnum=false
      }
    }
  };
  if(any==false){<false};
  <allnum
};
```

`str.tofloat` returns a result type, so `mt … {$ok:x true; $err:e false}`
turns a parse attempt into a boolean. A column is numeric only if it has at
least one value and every non-empty cell parses as a number.

### 6. compute statistics

```toke
f=tofloat(s:$str):f64{
  let v=mt str.tofloat(str.trim(s)) {
    $ok:x x;
    $err:e 0.0
  };
  <v
};

f=computecol(rows:@$csvrow;c:i64;ndata:i64;name:$str):$colstats{
  let count=mut.0;
  let sum=mut.0.0;
  let mn=mut.0.0;
  let mx=mut.0.0;
  let first=mut.true;
  lp(let r=0;r<ndata;r=r+1){
    let v=str.trim(cell(rows;r;c));
    if(str.len(v)>0){
      let num=tofloat(v);
      count=count+1;
      sum=sum+num;
      if(first){
        mn=num;
        mx=num;
        first=false
      }el{
        if(num<mn){mn=num};
        if(num>mx){mx=num}
      }
    }
  };
  let avg=mut.0.0;
  if(count>0){
    avg=sum/(count as f64)
  };
  <$colstats{
    name:name;
    count:(count as u64);
    sum:sum;
    min:mn;
    max:mx;
    avg:avg
  }
};
```

`(count as u64)` and `(count as f64)` — toke requires explicit casts between
integer and float types. The `first` flag seeds min/max with the first value
seen rather than a sentinel.

### 7. serialise to JSON

Toke builds JSON strings with a string buffer — `str.buf()` allocates,
`str.add()` appends, `str.done()` finalises. One function serialises a single
column; another wraps the whole report:

```toke
f=colstatstojson(s:$colstats):$str{
  let buf=str.buf();
  str.add(buf;"{\"name\":\"");
  str.add(buf;s.name);
  str.add(buf;"\",\"count\":");
  str.add(buf;str.fromint(s.count as i64));
  str.add(buf;",\"sum\":");
  str.add(buf;str.format(s.sum;"%.2f"));
  str.add(buf;",\"min\":");
  str.add(buf;str.format(s.min;"%.2f"));
  str.add(buf;",\"max\":");
  str.add(buf;str.format(s.max;"%.2f"));
  str.add(buf;",\"avg\":");
  str.add(buf;str.format(s.avg;"%.2f"));
  str.add(buf;"}");
  <str.done(buf)
};

f=reporttojson(r:$report):$str{
  let buf=str.buf();
  str.add(buf;"{\"filename\":\"");
  str.add(buf;r.filename);
  str.add(buf;"\",\"rows\":");
  str.add(buf;str.fromint(r.rows as i64));
  str.add(buf;",\"columns\":");
  str.add(buf;str.fromint(r.columns as i64));
  str.add(buf;",\"stats\":[");
  lp(let i=0;i<(r.stats.len as i64);i=i+1){
    if(i>0){str.add(buf;",")};
    str.add(buf;colstatstojson(r.stats.get(i)))
  };
  str.add(buf;"]}");
  <str.done(buf)
};
```

### 8. print summary and wire together

`printstats` prints one column to stdout; `main` orchestrates the whole
pipeline — read, parse, compute every numeric column, print the summary, then
write the JSON report to the output path:

```toke
f=printstats(s:$colstats):$i64{
  io.println(str.concat("  ";str.concat(s.name;str.concat(" (count ";str.concat(str.fromint(s.count as i64);")")))));
  io.println(str.concat("    sum=";str.format(s.sum;"%.2f")));
  io.println(str.concat("    avg=";str.format(s.avg;"%.2f")));
  io.println(str.concat("    min=";str.format(s.min;"%.2f")));
  io.println(str.concat("    max=";str.format(s.max;"%.2f")));
  <0
};

f=main():$i64{
  if(args.count()<3){
    io.eprintln("usage: datapipe <input.csv> <output.json>");
    <2
  };
  let path=args.get(1);
  let output=args.get(2);
  let raw=mt file.read(path) {
    $ok:v v;
    $err:e ""
  };
  if(str.len(raw)==0){
    io.eprintln(str.concat("cannot read file: ";path));
    <2
  };
  let rows=mt csv.parse(str.bytes(raw)) {
    $ok:v v;
    $err:e @()
  };
  if(rows.len<2){
    io.eprintln("no data rows in csv");
    <2
  };
  let headers=rows.get(0).fields;
  let ncols=headers.len as i64;
  let ndata=(rows.len as i64)-1;

  let stats=mut.@();
  lp(let c=0;c<ncols;c=c+1){
    if(iscolnumeric(rows;c;ndata)){
      stats=stats.push(computecol(rows;c;ndata;headers.get(c)))
    }
  };

  io.println(str.concat("file:    ";path));
  io.println(str.concat("rows:    ";str.fromint(ndata)));
  io.println(str.concat("columns: ";str.fromint(ncols)));
  io.println("");
  io.println("numeric column statistics:");
  lp(let i=0;i<(stats.len as i64);i=i+1){
    printstats(stats.get(i))
  };

  let report=$report{
    filename:path;
    rows:(ndata as u64);
    columns:(ncols as u64);
    stats:stats
  };
  let jsonstr=reporttojson(report);
  mt file.write(output;jsonstr) {
    $ok:v io.println(str.concat("\nreport written to: ";output));
    $err:e io.eprintln(str.concat("error writing report: ";output))
  };
  <0
};
```

The store of computed columns is built in a `mut.@()` array, pushing one
`$colstats` per numeric column. Reading the store, the summary print and the
JSON serialisation both iterate that same array.

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