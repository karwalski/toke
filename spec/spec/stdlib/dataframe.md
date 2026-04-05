# std.dataframe

Columnar data frames: load from CSV, query columns, filter, group, join, and export.

## Types

```
type dataframe { ncols: u64, nrows: u64, arena: u64 }  -- opaque
type series    { name: str, dtype: str, len: u64 }
type dfshape   { rows: u64, cols: u64 }
type dferr     { msg: str }
```

## Functions

```
f=df.fromcsv(data:[byte]):dataframe!dferr
f=df.fromrows(columns:[str];rows:[csvrow]):dataframe
f=df.column(d:dataframe;name:str):[f64]!dferr
f=df.columnstr(d:dataframe;name:str):[str]!dferr
f=df.filter(d:dataframe;col:str;op:str;val:str):dataframe!dferr
f=df.groupby(d:dataframe;col:str):[dataframe]!dferr
f=df.join(left:dataframe;right:dataframe;on:str):dataframe!dferr
f=df.head(d:dataframe;n:u64):dataframe
f=df.shape(d:dataframe):dfshape
f=df.tojson(d:dataframe):[byte]!dferr
f=df.tocsv(d:dataframe):[byte]
f=df.schema(d:dataframe):[series]
```

## Semantics

- `fromcsv` parses CSV bytes into a dataframe, inferring column types (numeric columns become f64, others remain str). Returns `dferr` on malformed CSV.
- `fromrows` builds a dataframe from explicit column names and `csvrow` data.
- `column` extracts a numeric column as `[f64]`. Returns `dferr` if the column does not exist or is not numeric. `columnstr` extracts as `[str]`.
- `filter` applies a comparison. `op` is one of `"=="`, `"!="`, `">"`, `"<"`, `">="`, `"<="`. `val` is compared as the column's native type.
- `groupby` splits the dataframe into one dataframe per distinct value in the named column.
- `join` performs an inner join on the named column.
- `head` returns the first `n` rows.
- `shape` returns row and column counts.
- `tojson` serializes to JSON array-of-objects. `tocsv` serializes back to CSV bytes.
- `schema` returns column metadata (name, inferred dtype, length).

## Dependencies

- `std.csv` (CSV parsing for `fromcsv` and `csvrow` type)
