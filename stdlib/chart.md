# std.chart -- Data Visualization

## Overview

The `std.chart` module converts data arrays into chart specification structs and serialises them to Chart.js-compatible or Vega-Lite-compatible JSON. Rendering is always client-side; `std.chart` only produces the descriptor bytes. Pair with `std.html` to embed charts in a page or `std.dashboard` for a live dashboard.

The typical pipeline is: build one or more `dataset` values, pass them to a chart constructor (`chart.bar`, `chart.line`, `chart.scatter`, or `chart.pie`) to obtain a `chartspec`, then call `chart.tojson` or `chart.tovega` to emit the JSON bytes for the target rendering library.

## Types

### dataset

A single data series within a multi-series chart.

| Field | Type | Meaning |
|-------|------|---------|
| label | Str | Series name shown in the legend |
| data | @f64 | Data values |
| color | Str | CSS colour string (e.g., `"#4e79a7"`) |

**Example:**
```toke
let ds = $dataset{label: "Revenue"; data: @(120.0; 95.5; 140.2); color: "#4e79a7"};
```

### chartspec

A complete chart description ready for serialisation.

| Field | Type | Meaning |
|-------|------|---------|
| type | Str | Chart type: `"bar"`, `"line"`, `"scatter"`, or `"pie"` |
| labels | @Str | Category labels (x-axis or slice labels) |
| datasets | @dataset | One or more data series |
| title | Str | Chart title |

### charterr

Returned when chart construction fails (e.g., mismatched array lengths).

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Error description |

## Functions

### chart.bar(labels: @Str; values: @f64; title: Str): chartspec

Creates a bar chart spec with a single dataset. Each element in `values` corresponds to the label at the same index in `labels`. A default colour is assigned automatically.

**Example:**
```toke
let spec = chart.bar(
    @("Jan"; "Feb"; "Mar");
    @(120.0; 95.5; 140.2);
    "Monthly Revenue"
);
```

### chart.line(labels: @Str; datasets: @dataset; title: Str): chartspec

Creates a line chart spec with one or more series. Each `dataset` supplies its own colour and legend label; the shared `labels` array provides the x-axis categories. Use multiple datasets to overlay trends on the same axes.

**Example:**
```toke
let sales = $dataset{label: "Sales"; data: @(10.0; 20.0; 30.0); color: "#4e79a7"};
let costs = $dataset{label: "Costs"; data: @(8.0; 15.0; 22.0); color: "#e15759"};
let spec = chart.line(@("Q1"; "Q2"; "Q3"); @(sales; costs); "Quarterly Breakdown");
```

### chart.scatter(xs: @f64; ys: @f64; label: Str): chartspec

Creates a scatter chart from paired x/y coordinates. The `xs` and `ys` arrays must be the same length; element `i` in each array forms the point `(xs[i], ys[i])`. The `label` is used as the series name.

**Example:**
```toke
let spec = chart.scatter(
    @(1.0; 2.0; 3.0; 4.0);
    @(2.1; 3.9; 6.2; 7.8);
    "Correlation"
);
```

### chart.pie(labels: @Str; values: @f64; title: Str): chartspec

Creates a pie chart spec. Each value is rendered as a proportional slice; the module assigns distinct colours per slice automatically. Negative values are clamped to zero.

**Example:**
```toke
let spec = chart.pie(
    @("Red"; "Blue"; "Green");
    @(40.0; 35.0; 25.0);
    "Market Share"
);
```

### chart.tojson(spec: chartspec): @byte

Serialises `spec` to Chart.js-compatible JSON bytes. The output is a complete configuration object that can be passed directly to `new Chart(ctx, JSON.parse(...))` on the client side.

**Example:**
```toke
let bytes = chart.tojson(spec);
file.write("/tmp/chart.json"; str.frombytes(bytes));
```

### chart.tovega(spec: chartspec): @byte

Serialises `spec` to Vega-Lite-compatible JSON bytes. The output is a top-level Vega-Lite specification with `$schema`, `mark`, `encoding`, and `data` fields. Use this when targeting Vega-Lite renderers instead of Chart.js.

**Example:**
```toke
let bytes = chart.tovega(spec);
file.write("/tmp/vega.json"; str.frombytes(bytes));
```

## Error Types

### charterr

Returned when chart operations encounter invalid input, such as mismatched array lengths between `labels` and `values`, or empty data arrays.

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Human-readable description of the error |

## Pipeline Example -- Data to Chart to File

The following program reads sales data, builds a multi-series line chart, and writes both Chart.js and Vega-Lite JSON files.

```toke
m=sales_chart;

f=main(): i64 {
    (* -- 1. Define the data -- *)
    let months = @("Jan"; "Feb"; "Mar"; "Apr"; "May"; "Jun");

    let online = $dataset{
        label: "Online";
        data: @(42.0; 51.0; 60.5; 55.0; 72.3; 68.0);
        color: "#4e79a7"
    };
    let retail = $dataset{
        label: "Retail";
        data: @(30.0; 28.5; 35.0; 40.0; 38.0; 45.5);
        color: "#e15759"
    };

    (* -- 2. Build the chart spec -- *)
    let spec = chart.line(months; @(online; retail); "Sales by Channel");

    (* -- 3. Export to JSON -- *)
    let chartjs = chart.tojson(spec);
    file.write("/tmp/sales_chartjs.json"; str.frombytes(chartjs));

    let vega = chart.tovega(spec);
    file.write("/tmp/sales_vega.json"; str.frombytes(vega));

    < 0
};
```

## Quick-Reference Table

| Function | Returns | Purpose |
|----------|---------|---------|
| `chart.bar` | `chartspec` | Single-series bar chart |
| `chart.line` | `chartspec` | Multi-series line chart |
| `chart.scatter` | `chartspec` | X/Y scatter plot |
| `chart.pie` | `chartspec` | Proportional pie chart |
| `chart.tojson` | `@byte` | Emit Chart.js JSON |
| `chart.tovega` | `@byte` | Emit Vega-Lite JSON |

## See Also

- `std.svg` -- for full programmatic vector graphics control
- `std.html` -- for embedding charts in web pages
- `std.dashboard` -- for live dashboards with auto-refresh
