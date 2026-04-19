# Datapipe

A CSV data processing pipeline demonstrating file I/O, parsing, and data
transformation in toke.

## Build

```bash
tkc main.tk -o datapipe
```

## Usage

```
$ ./datapipe <input.csv> <output.csv>
```

Example:

```
$ ./datapipe sales.csv summary.csv
Read 1024 rows from sales.csv
Wrote 12 rows to summary.csv
```

## Features

- CSV parsing and writing
- Data filtering and aggregation
- Pipeline-style data transformation
- File I/O with error handling

## Tutorial

See [Data Pipeline Tutorial](/docs/tutorials/data-pipeline/) for a step-by-step
guide.
