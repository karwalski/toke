---
title: std.chart
slug: chart
section: reference/stdlib
order: 5
---

**Status: Implemented** -- C runtime backing.

The `std.chart` module provides a declarative API for building chart specifications. Create bar, line, scatter, and pie charts, then export them as JSON or Vega-Lite for rendering in dashboards or HTML pages.

## Types

### $dataset

| Field | Type | Meaning |
|-------|------|---------|
| label | $str | Series label |
| data | @(f64) | Data points |
| color | $str | CSS color string; `""` for the default palette (as `chart.bar` builds it, with label `"data"`) |

### $chartspec

| Field | Type | Meaning |
|-------|------|---------|
| type | $str | Chart type (`"bar"`, `"line"`, `"scatter"`, `"pie"`) |
| labels | @($str) | Category labels (x-axis) |
| datasets | @($dataset) | One or more data series |
| title | $str | Chart title |

### $charterr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `chart.bar` | `labels: @($str); values: @(f64); title: $str` | `$chartspec` | Create a bar chart with one series |
| `chart.line` [quarantined](/docs/reference/tki-quarantine/) | `labels: @($str); datasets: @($dataset); title: $str` | `$chartspec` | Create a line chart with multiple series |
| `chart.scatter` [quarantined](/docs/reference/tki-quarantine/) | `x: @(f64); y: @(f64); title: $str` | `$chartspec` | Create a scatter plot |
| `chart.pie` [quarantined](/docs/reference/tki-quarantine/) | `labels: @($str); values: @(f64); title: $str` | `$chartspec` | Create a pie chart |
| `chart.tojson` | `spec: $chartspec` | `@($byte)` | Serialize chart spec to JSON bytes |
| `chart.tovega` [quarantined](/docs/reference/tki-quarantine/) | `spec: $chartspec` | `@($byte)` | Serialize chart spec to Vega-Lite JSON bytes |

## Escaping

Titles, labels and series names are JSON-escaped in the Chart.js and
Vega-Lite output; a NaN or infinite data value is written as `null` (a gap),
since neither is a JSON number.

## Usage

```toke
m=example;
i=chart:std.chart;

f=main():i64{
  let c = chart.bar(
    @("Q1"; "Q2"; "Q3"; "Q4");
    @(100.0; 200.0; 150.0; 300.0);
    "Quarterly Revenue"
  );
  let json = chart.tojson(c);
  <0;
}
```

## Dependencies

None.
