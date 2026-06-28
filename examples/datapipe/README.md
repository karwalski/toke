# Datapipe

A CSV data-processing pipeline: read a CSV, detect the numeric columns, and
report per-column statistics (count, sum, avg, min, max).

## Build

```bash
tkc --out datapipe main.tk
```

## Usage

```
$ ./datapipe <input.csv>
```

Example:

```
$ ./datapipe sales.csv
file:    sales.csv
rows:    3
columns: 3

numeric column statistics:
  price (count 3)
    sum=33.74
    avg=11.25
    min=4.25
    max=19.50
  qty (count 3)
    sum=350.00
    avg=116.67
    min=50.00
    max=200.00
```

## Features

- CSV parsing (`csv.parse` → `[csvrow]`)
- Numeric-column detection via `str.tofloat`
- Per-column aggregation (count/sum/avg/min/max)
- File I/O with error handling (`mt file.read {…}`)

## Tutorial

See [Data Pipeline Tutorial](/docs/tutorials/data-pipeline/) for a step-by-step
guide.
