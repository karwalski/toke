# std.chart -- Data Visualization

## Overview

The `std.chart` module converts data arrays into chart specification structs and serialises them to Chart.js-compatible or Vega-Lite-compatible JSON. Rendering is always client-side; `std.chart` only produces the descriptor bytes. Pair with `std.html` to embed charts in a page or `std.dashboard` for a live dashboard.

## Types

### dataset

A single data series within a multi-series chart.

| Field | Type | Meaning |
|-------|------|---------|
| label | Str | Series name shown in the legend |
| data | @f64 | Data values |
| color | Str | CSS colour string (e.g., `"#4e79a7"`) |

### chartspec

A complete chart description.

| Field | Type | Meaning |
|-------|------|---------|
| type | Str | Chart type: `"bar"`, `"line"`, `"scatter"`, or `"pie"` |
| labels | @Str | Category labels (x-axis or slice labels) |
| datasets | @dataset | One or more data series |
| title | Str | Chart title |

### charterr

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Error description |

## Functions

### chart.bar(labels: @Str; values: @f64; title: Str): chartspec

Creates a bar chart spec with a single dataset.

**Example:**
```toke
let spec = chart.bar(@("Jan"; "Feb"; "Mar"); @(120.0; 95.5; 140.2); "Monthly Revenue");
```

### chart.line(labels: @Str; datasets: @dataset; title: Str): chartspec

Creates a line chart spec with one or more series.

**Example:**
```toke
let ds = $dataset{label: "Sales"; data: @(10.0; 20.0; 30.0); color: "#4e79a7"};
let spec = chart.line(@("Q1"; "Q2"; "Q3"); @(ds); "Quarterly Sales");
```

### chart.scatter(xs: @f64; ys: @f64; label: Str): chartspec

Creates a scatter chart from paired x/y coordinates.

**Example:**
```toke
let spec = chart.scatter(@(1.0; 2.0; 3.0); @(2.1; 3.9; 6.2); "Correlation");
```

### chart.pie(labels: @Str; values: @f64; title: Str): chartspec

Creates a pie chart spec.

**Example:**
```toke
let spec = chart.pie(@("Red"; "Blue"; "Green"); @(40.0; 35.0; 25.0); "Market Share");
```

### chart.tojson(spec: chartspec): @byte

Serialises `spec` to Chart.js-compatible JSON bytes.

**Example:**
```toke
let bytes = chart.tojson(spec);
file.write("/tmp/chart.json"; str.frombytes(bytes));
```

### chart.tovega(spec: chartspec): @byte

Serialises `spec` to Vega-Lite-compatible JSON bytes.

**Example:**
```toke
let bytes = chart.tovega(spec);
```
