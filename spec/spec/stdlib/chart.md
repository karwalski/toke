# std.chart

Declarative chart specification: bar, line, scatter, and pie charts with JSON and Vega-Lite export.

## Types

```
type dataset   { label: str, data: [f64], color: str }
type chartspec { type: str, labels: [str], datasets: [dataset], title: str }
type charterr  { msg: str }
```

## Functions

```
f=bar(labels:[str];values:[f64];title:str):chartspec
f=line(labels:[str];datasets:[dataset];title:str):chartspec
f=scatter(x:[f64];y:[f64];title:str):chartspec
f=pie(labels:[str];values:[f64];title:str):chartspec
f=tojson(spec:chartspec):[byte]
f=tovega(spec:chartspec):[byte]
```

## Semantics

- `bar` creates a single-dataset bar chart. `labels` and `values` must be equal length.
- `line` supports multiple datasets over shared x-axis labels.
- `scatter` creates an x/y scatter plot from paired arrays of equal length.
- `pie` creates a pie chart. `labels` and `values` must be equal length.
- `tojson` serializes the chart spec to a portable JSON representation.
- `tovega` serializes the chart spec to a Vega-Lite JSON specification, suitable for rendering in browsers or notebooks.
- `dataset.color` accepts CSS color strings (e.g., `"#ff0000"`, `"steelblue"`).

## Dependencies

None.
