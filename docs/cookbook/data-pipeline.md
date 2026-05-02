---
title: Cookbook — Data pipeline (CSV analysis)
slug: data-pipeline
section: reference/cookbook
order: 1
---

A data processing pipeline that reads a CSV file, splits each row into fields, filters on a target column value, counts occurrences per category, and writes a plain-text summary to disk. The program uses only `std.file` and `std.str` — no external data-processing library — to show how toke's string primitives handle tabular data directly.

## What this demonstrates

- Reading a text file with `file.read` and splitting it into rows
- Parsing CSV fields with `str.split`
- Filtering rows with `if` / `el`
- Accumulating counts across loop iterations with mutable variables
- Building a multi-line report with `str.concat`
- Writing the result to disk with `file.write`
- Error propagation chained across multiple stdlib calls

## The program

```toke
m=datapipeline;

i=file:std.file;
i=str:std.str;
i=log:std.log;

t=$err{code:i64};

f=getfield(row:$str;col:i64):$str{
  let fields=str.split(row; ",");
  if(col<fields.len as i64){
    < fields.get(col)
  }el{
    < ""
  }
};

f=countmatches(rows:@$str;col:i64;target:$str):i64{
  let count=mut.0;
  lp(let i=0;i<rows.len as i64;i=i+1){
    let val=str.trim(getfield(rows.get(i); col));
    if(val=target){
      count=count+1
    }
  };
  < count
};

f=buildreport(target:$str;total:i64;cnt:i64):$str{
  let pct=cnt*100/total;
  let line1=str.concat("filter: "; target);
  let line2=str.concat("total rows: "; str.fromInt(total));
  let line3=str.concat("matching: "; str.fromInt(cnt));
  let line4=str.concat("percent: "; str.concat(str.fromInt(pct); "%"));
  < str.concat(line1;
    str.concat("\n";
      str.concat(line2;
        str.concat("\n";
          str.concat(line3;
            str.concat("\n"; line4))))))
};

f=run(csvpath:$str;col:i64;target:$str;outpath:$str):i64!$err{
  let raw=file.read(csvpath)!$err;
  let allrows=str.split(raw; "\n");
  let datarows=mut.0;
  lp(let i=1;i<allrows.len as i64;i=i+1){
    let row=str.trim(allrows.get(i));
    if(!(row="")){ datarows=datarows+1 }
  };
  let matched=countmatches(allrows; col; target);
  let report=buildreport(target; datarows; matched);
  log.info(report);
  file.write(outpath; report)!$err;
  log.info(str.concat("report written to "; outpath));
  < 0
};

f=main():i64!$err{
  run("data/sales.csv"; 2; "active"; "out/summary.txt")!$err;
  < 0
};
```

## Walking through the code

**`f=getfield`** — Splits a CSV row by comma and returns the field at the requested column index. The bounds check (`if(col<fields.len as i64)`) compares the signed `col` parameter against the unsigned `.len` (cast to `i64`). Returns an empty string for missing fields rather than panicking, making the function safe to call on ragged rows.

**`f=countmatches`** — Iterates all rows, extracts the target column with `getfield`, trims whitespace with `str.trim`, and increments `count` when the value equals `target`. `count` is declared mutable before the loop; the loop variable `i` is declared inside the `lp` init expression (`let i=0`) so it is scoped to the loop. The `lp` step expression (`i=i+1`) handles advancing `i` each iteration — there is no separate increment inside the body.

**`f=buildreport`** — Computes a rough percentage (integer division) and assembles a four-line report string. Each line is bound to a `let` variable before being concatenated, which keeps the nesting depth manageable.

**`f=run`** — Orchestrates the pipeline:

1. Reads the CSV file; propagates any IO error with `!$err`.
2. Splits on newline to get all rows, then counts non-empty data rows (skipping the header at index 0 and any trailing blank lines).
3. Calls `countmatches` to count rows where column `col` equals `target`.
4. Builds and logs the report, then writes it to `outpath`.

**`f=main`** — Hard-codes the example: look at column 2 (`status`) of `data/sales.csv` and count `"active"` rows. Adjust the arguments to match your dataset.

## Sample input

`data/sales.csv`:

```
id,name,status,amount
1,alpha,active,120
2,beta,closed,45
3,gamma,active,88
4,delta,active,200
5,epsilon,closed,30
```

## Running the program

```shell
mkdir -p out
tkc datapipeline.tk
./datapipeline
```

`out/summary.txt`:

```
filter: active
total rows: 5
matching: 3
percent: 60%
```

## Key syntax reminders

| Pattern | Syntax |
|---------|--------|
| Inequality | `!(a=b)` (no `!=` operator) |
| Mutable variable | `let x=mut.0;` then `x=x+1` |
| Skip header row | start loop at `i=1` |
| Trim whitespace | `str.trim(s)` |
| Integer percent | `cnt*100/total` |
| Write file | `file.write(path; content)!$err` |
