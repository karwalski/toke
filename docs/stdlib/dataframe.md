---
title: std.dataframe
slug: dataframe
section: reference/stdlib
order: 10
---

**Status: Implemented** -- C runtime backing.

The `std.dataframe` module provides a columnar data frame for tabular data processing. Load data from CSV text or row arrays, then filter, group, join, slice, and export. Columns where every value parses as a 64-bit float are stored as `f64`; all others are stored as `$str`.

## Types

### $dataframe

Opaque handle to a columnar data frame. The frame owns all column memory; release it when no longer needed. It has no readable fields: use `df.shape` for the row and column counts and the other `df.*` functions for the rest. The interface declares `ncols`, `nrows` and `arena` so a record literal can be written, but reading one is a compile error, E4034 (138.65; until then `d.ncols` read the column array's address and `d.nrows` the column count). A value no `df.*` or `analytics.*` function returned -- a record literal, `5 as $dataframe` -- is the null frame: the fallible calls return `$err`, `df.tocsv` returns `""` and `df.shape` returns `{rows:0;cols:0}` (138.63).

### $series

Describes a single column returned by `df.schema`.

| Field | Type | Meaning |
|-------|------|---------|
| name | $str | Column name as it appears in the header row |
| dtype | $str | Storage type: `"f64"` for numeric columns, `"str"` for text |
| len | u64 | Number of values in the column (equals the frame's row count) |

### $dfshape

Compact row/column count returned by `df.shape`.

| Field | Type | Meaning |
|-------|------|---------|
| rows | u64 | Number of rows |
| cols | u64 | Number of columns |

### $dferr

Error value returned when a data frame operation fails.

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Human-readable description of the failure |

## Functions

### df.fromcsv(data: $str): $dataframe!$dferr

Parses CSV text into a new data frame, treating the first row as column headers. Columns where every cell parses as a finite 64-bit float are stored as `f64`; all other columns are stored as `$str`. Returns `$dferr` if the input is not valid CSV or if memory allocation fails.

<!-- skip-check -->
```toke
let raw  = mt file.read("sales.csv") {$ok:d d;$err:e <1};
let data = mt df.fromcsv(raw) {$ok:d d;$err:e <1};
```

### df.fromrows(cols: @($str); rows: @($csvrow)): $dataframe

Constructs a data frame from an explicit list of column names and row-major string data. Each element of `rows` must have the same length as `cols`; columns where every value parses as `f64` are stored numerically. This function does not fail -- if `rows` is empty the result is a frame with zero rows.

<!-- skip-check -->
```toke
let headers = @("name"; "score");
let data    = @(@("alice"; "92"); @("bob"; "87"));
let frame   = df.fromrows(headers; data);
```

### df.column(d: $dataframe; name: $str): @(f64)!$dferr

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Extracts a numeric column by name and returns its values as an `f64` array. Returns `$dferr` if the column is not found or was stored as `$str` rather than `f64`.

<!-- skip-check -->
```toke
let revenue = mt df.column(data; "revenue") {$ok:v v;$err:e @()};
```

### df.columnstr(d: $dataframe; name: $str): @($str)!$dferr

Extracts a string column by name and returns its values as a `$str` array, one per row in row order (a frame with no rows gives `@()`). Returns `$dferr` if the column is not found or was stored as `f64` rather than `$str`.

<!-- skip-check -->
```toke
let regions = mt df.columnstr(data; "region") {$ok:v v;$err:e @()};
```

### df.filter(d: $dataframe; col: $str; op: $str; val: $str): $dataframe!$dferr

Returns a new data frame containing only the rows where the numeric column `col` satisfies `col op val`. The `op` argument must be exactly one of `"<"`, `"<="`, `"="`, `">="`, `">"`, or `"!="`. `val` is a number written as text (`"2"`, `"2.5"`, `"-1"`, `"1e3"`), read in full as an `f64` with the C `strtod` number syntax, so hex floats (`"0x1p1"`), `"nan"` and `"inf"` are also accepted, as they are when `df.fromcsv` decides a column is numeric. Returns `$dferr` if `col` does not exist or is not a numeric column, if `op` is anything else (`"=="` and `" <"` included), or if `val` is empty, starts with whitespace (`" 2"`), or has anything after the number (`"2x"`, `"2 "`) (138.66).

<!-- skip-check -->
```toke
let apac  = mt df.filter(data; "region_id"; ">="; "100") {$ok:d d;$err:e <1};
let large = mt df.filter(apac; "revenue";   ">";  "5000") {$ok:d d;$err:e <1};
```

### df.groupby(d: $dataframe; col: $str): @($dataframe)!$dferr

Splits the data frame into one sub-frame per distinct value in the string column `col`, returning them as an array. Each sub-frame has every column of `d` and contains all rows that share the same group key value, in their original order; the groups are ordered by the first row in which each value appears. Each element is a `$dataframe` like any other: `df.shape`, `df.tocsv`, `df.filter` and `df.groupby` all accept it. A frame with no rows gives an empty array. Returns `$dferr` if `col` is not found or is not a `$str` column.

<!-- skip-check -->
```toke
let groups = mt df.groupby(data; "region") {$ok:g g;$err:e <1};
```

### df.join(left: $dataframe; right: $dataframe; on: $str): $dataframe!$dferr

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Performs an inner hash join on the string column `on`, which must exist in both frames. Rows with matching key values are combined; unmatched rows are excluded. The key column appears exactly once in the result. Returns `$dferr` if `on` is absent from either frame or is not a `$str` column.

<!-- skip-check -->
```toke
let combined = mt df.join(orders; customers; "customer_id") {$ok:d d;$err:e <1};
```

### df.head(d: $dataframe; n: u64): $dataframe

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Returns a new data frame containing at most the first `n` rows of `d`. If `n` is greater than or equal to the number of rows, the entire frame is returned. This function never fails.

<!-- skip-check -->
```toke
let preview = df.head(data; 5);
```

### df.shape(d: $dataframe): $dfshape

Returns the row and column counts without allocating. Use this to validate a frame before expensive operations.

<!-- skip-check -->
```toke
let s = df.shape(data);
```

### df.tocsv(d: $dataframe): $str

Serialises the data frame as RFC 4180 CSV with a header row and returns the result as text. This function never fails; an empty frame produces a header-only CSV.

<!-- skip-check -->
```toke
let text = df.tocsv(result);
mt file.write("output.csv"; text) {$ok:v ();$err:e <1};
```

### df.tojson(d: $dataframe): @($byte)!$dferr

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Serialises the data frame as a JSON array of row objects, with numeric columns emitting number values and string columns emitting quoted strings. Returns `$dferr` only on allocation failure.

<!-- skip-check -->
```toke
let json = mt df.tojson(summary) {$ok:b b;$err:e <1};
```

### df.schema(d: $dataframe): @($series)

> [quarantined](/docs/reference/tki-quarantine/): declared in the interface, but no C symbol implements it, so `tkc --check` rejects a call to it.

Returns an array of `$series` values describing every column in the frame -- its name, data type, and length. This function never fails; an empty frame returns an empty array.

<!-- skip-check -->
```toke
let cols = df.schema(data);
```

## Complete Pipeline Example

The example below shows a full CSV ingest → filter → group → aggregate → export pipeline.

```toke
m=main;
i=df:std.dataframe;
i=file:std.file;
i=io:std.io;
i=str:std.str;

f=main():i64{
  let raw = mt file.read("sales.csv") {$ok:d d;$err:e ""};

  let data = mt df.fromcsv(raw) {$ok:d d;$err:e <1};

  let s = df.shape(data);
  io.println(str.concat("Rows: "; str.fromint(s.rows)));

  let big = mt df.filter(data; "revenue"; ">"; "1000") {$ok:d d;$err:e <1};

  let groups = mt df.groupby(big; "region") {$ok:g g;$err:e @()};

  lp(let i=0;i<groups.len;i=i+1){
    let grp = groups.get(i);
    let regioncol = mt df.columnstr(grp; "region") {$ok:v v;$err:e @()};
    let out = df.tocsv(grp);
    let fname = str.concat(regioncol.get(0); ".csv");
    let wr = file.write(fname; out);
    mt wr {$ok:v 0;$err:e 0};
  };

  <0;
}
```

## See Also

- [std.csv](/docs/stdlib/csv) -- low-level CSV parsing and `$csvrow` type used by `df.fromrows`
- [std.analytics](/docs/stdlib/analytics) -- descriptive statistics, anomaly detection, and pivot tables built on `$dataframe`
