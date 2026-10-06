# Datapipe

A CSV data-processing pipeline: read a CSV, detect the numeric columns, and
report per-column statistics (count, sum, avg, min, max).

## Build

```bash
tkc --out datapipe main.tk
```

## Usage

```
$ ./datapipe --allow-read --allow-write <input.csv> <output.json>
```

toke programs run with no file access by default. `--allow-read` lets the
program read the CSV and `--allow-write` lets it write the JSON report;
without them it stops with a `CAP001` message naming the missing flag.

Example, with this `sales.csv`:

```
item,price,qty
widget,9.99,100
gadget,19.50,50
bolt,4.25,200
```

```
$ ./datapipe --allow-read --allow-write sales.csv report.json
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

report written to: report.json
```

## Features

- CSV parsing (`csv.parse` → `[csvrow]`)
- Numeric-column detection via `str.tofloat`
- Per-column aggregation (count/sum/avg/min/max)
- File I/O with error handling (`mt file.read {…}`)

## Tutorial

See [Data Pipeline Tutorial](/docs/tutorials/data-pipeline/) for a step-by-step
guide.
