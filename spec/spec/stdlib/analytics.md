# std.analytics

Descriptive statistics, group aggregation, time-series analysis, anomaly detection, pivoting, and correlation over dataframes.

## Types

```
type statsrow  { col: str, count: u64, mean: f64, stddev: f64, min: f64, p25: f64, p50: f64, p75: f64, max: f64 }
type groupstat { group: str, count: u64, sum: f64, mean: f64 }
type tspoint   { ts: u64, value: f64, rolling_mean: f64 }
```

## Functions

```
f=describe(d:dataframe):[statsrow]!dferr
f=groupstats(d:dataframe;groupcol:str;valuecol:str):[groupstat]!dferr
f=timeseries(d:dataframe;tscol:str;valuecol:str;window:u64):[tspoint]!dferr
f=anomalies(d:dataframe;col:str;threshold:f64):[u64]!dferr
f=pivot(d:dataframe;rowcol:str;colcol:str;valuecol:str):dataframe!dferr
f=corr(d:dataframe;col1:str;col2:str):f64!dferr
```

## Semantics

- `describe` computes summary statistics (count, mean, stddev, min, percentiles, max) for every numeric column.
- `groupstats` groups by `groupcol` and aggregates `valuecol` with count, sum, and mean.
- `timeseries` extracts a time series from columns `tscol` (unix timestamp) and `valuecol`, computing a rolling mean with the given `window` size.
- `anomalies` returns row indices where the value in `col` deviates from the mean by more than `threshold` standard deviations.
- `pivot` reshapes the dataframe: unique values of `colcol` become columns, `rowcol` values become rows, cells are `valuecol` values.
- `corr` computes the Pearson correlation coefficient between two numeric columns.
- All functions return `dferr` if a named column does not exist or is not the expected type.

## Dependencies

- `std.dataframe` (dataframe and dferr types)
- `std.math` (statistical primitives)
