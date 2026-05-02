---
title: std.analytics
slug: analytics
section: reference/stdlib
order: 1
---

**Status: Implemented** -- C runtime backing.

The `std.analytics` module provides higher-level analytical functions over `$dataframe` values. Compute per-column summary statistics, group aggregations, time series with rolling averages, z-score anomaly detection, pivot tables, and Pearson correlation. All functions that can fail return `!$dferr`.

## Types

### $statsrow

One row of summary statistics for a single numeric column, returned by `analytics.describe`.

| Field | Type | Meaning |
|-------|------|---------|
| col | $str | Column name |
| count | u64 | Number of non-null values |
| mean | f64 | Arithmetic mean |
| stddev | f64 | Population standard deviation |
| min | f64 | Minimum value |
| p25 | f64 | 25th percentile |
| p50 | f64 | 50th percentile (median) |
| p75 | f64 | 75th percentile |
| max | f64 | Maximum value |

### $groupstat

Per-group aggregation result returned by `analytics.groupstats`.

| Field | Type | Meaning |
|-------|------|---------|
| group | $str | Distinct value of the group key column |
| count | u64 | Number of rows in the group |
| sum | f64 | Sum of the value column within the group |
| mean | f64 | Mean of the value column within the group |

### $tspoint

One bucket of a time series returned by `analytics.timeseries`.

| Field | Type | Meaning |
|-------|------|---------|
| ts | u64 | Bucket start time as Unix epoch milliseconds |
| value | f64 | Mean (or count) of the value column within this bucket |
| rollingmean | f64 | Trailing rolling average centred on this bucket |

## Functions

### analytics.describe(d: $dataframe): @($statsrow)!$dferr

Computes count, mean, standard deviation, minimum, three quartiles (p25/p50/p75), and maximum for every numeric (`f64`) column in `d`. String columns are silently skipped. Returns `$dferr` if allocation fails.

```toke
m=example;
i=analytics:std.analytics;
i=df:std.dataframe;
i=file:std.file;

f=main():i64{
  let raw=file.read("data.csv")|{$ok:d d;$err:e ""};
  let data=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let stats=analytics.describe(data)|{$ok:s s;$err:e @()};
  <0;
};
```

### analytics.corr(d: $dataframe; cola: $str; colb: $str): f64!$dferr

Computes the Pearson product-moment correlation coefficient between two numeric columns. The result is in the range `[-1.0, 1.0]`; values close to `1.0` indicate a strong positive linear relationship, values close to `-1.0` a strong negative relationship, and values near `0.0` suggest no linear association. Returns `$dferr` if either column is absent or is not numeric.

```toke
m=example;
i=analytics:std.analytics;
i=df:std.dataframe;
i=file:std.file;

f=main():i64{
  let raw=file.read("data.csv")|{$ok:d d;$err:e ""};
  let data=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let r=analytics.corr(data;"cpu";"latency")|{$ok:v v;$err:e 0.0};
  <0;
};
```

### analytics.anomalies(d: $dataframe; col: $str; threshold: f64): @(u64)!$dferr

Detects rows in the numeric column `col` whose z-score exceeds `threshold` in absolute value, returning their zero-based row indices. A threshold of `2.0` flags values more than two standard deviations from the mean; `3.0` is a stricter cutoff that catches only extreme outliers. Returns `$dferr` if `col` is absent or is not numeric.

```toke
m=example;
i=analytics:std.analytics;
i=df:std.dataframe;
i=file:std.file;

f=main():i64{
  let raw=file.read("data.csv")|{$ok:d d;$err:e ""};
  let data=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let outliers=analytics.anomalies(data;"latency";3.0)|{$ok:a a;$err:e @()};
  <0;
};
```

### analytics.groupstats(d: $dataframe; groupcol: $str; valuecol: $str): @($groupstat)!$dferr

Groups the data frame by each distinct value in the string column `groupcol`, then computes count, sum, and mean of the numeric column `valuecol` within each group. The output array has one `$groupstat` entry per distinct group key, in the order the keys are first encountered. Returns `$dferr` if either column is absent or has the wrong type.

```toke
m=example;
i=analytics:std.analytics;
i=df:std.dataframe;
i=file:std.file;

f=main():i64{
  let raw=file.read("data.csv")|{$ok:d d;$err:e ""};
  let data=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let gs=analytics.groupstats(data;"region";"revenue")|{$ok:g g;$err:e @()};
  <0;
};
```

### analytics.pivot(d: $dataframe; rowcol: $str; colcol: $str; valuecol: $str): $dataframe!$dferr

Builds a pivot table from `d`. Unique values of the string column `rowcol` become row keys; unique values of the string column `colcol` become column headers; cells are filled with the sum of `valuecol` for each matching `(rowcol, colcol)` pair. Returns `$dferr` if any named column is absent or has the wrong type.

```toke
m=example;
i=analytics:std.analytics;
i=df:std.dataframe;
i=file:std.file;

f=main():i64{
  let raw=file.read("data.csv")|{$ok:d d;$err:e ""};
  let data=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let table=analytics.pivot(data;"region";"product";"revenue")|{$ok:t t;$err:e data};
  <0;
};
```

### analytics.timeseries(d: $dataframe; tscol: $str; valuecol: $str; window: u64): @($tspoint)!$dferr

Aggregates values into fixed-width time buckets of `window` milliseconds using the numeric column `tscol` (Unix epoch ms) for ordering. Each bucket holds the mean of `valuecol` values that fall within it. Returns `$dferr` if either column is absent or is not numeric.

```toke
m=example;
i=analytics:std.analytics;
i=df:std.dataframe;
i=file:std.file;

f=main():i64{
  let raw=file.read("logs.csv")|{$ok:d d;$err:e ""};
  let logs=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let series=analytics.timeseries(logs;"ts";"latency";60000)|{$ok:s s;$err:e @()};
  <0;
};
```

## Anomaly Detection Example

The example below loads a metrics CSV, detects anomalous latency readings, then writes a filtered report containing only the outlier rows.

```toke
m=example;
i=df:std.dataframe;
i=analytics:std.analytics;
i=file:std.file;
i=io:std.io;
i=str:std.str;

f=main():i64{
  let raw=file.read("metrics.csv")|{$ok:d d;$err:e ""};
  let data=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let idx=analytics.anomalies(data;"latency";3.0)|{$ok:a a;$err:e @()};
  io.println(str.concat("Anomalies: ";str.fromint(idx.len as i64)));
  let flagged=df.filter(data;"anomalyflag";"=";"1")|{$ok:d d;$err:e data};
  let out=df.tocsv(flagged);
  let res=file.write("anomalies.csv";out);
  <0;
};
```

## Clustering Example

The example below groups sensors by region, computes per-group statistics, then prints a summary of each cluster.

```toke
m=example;
i=df:std.dataframe;
i=analytics:std.analytics;
i=file:std.file;
i=io:std.io;
i=str:std.str;

f=main():i64{
  let raw=file.read("sensors.csv")|{$ok:d d;$err:e ""};
  let data=df.fromcsv(raw)|{$ok:d d;$err:e df.fromrows(@();@())};
  let gs=analytics.groupstats(data;"region";"reading")|{$ok:g g;$err:e @()};
  lp(let i=0;i<gs.len;i=i+1){
    let row=gs.get(i);
    let line=str.concat(row.group;str.concat(": mean=";str.fromfloat(row.mean)));
    io.println(line);
  };
  <0;
};
```

## See Also

- [std.dataframe](/docs/stdlib/dataframe) -- `$dataframe` type and core frame operations required by all analytics functions
- [std.math](/docs/stdlib/math) -- low-level numeric and statistical primitives underpinning this module
