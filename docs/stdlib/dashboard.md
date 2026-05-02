---
title: std.dashboard
slug: dashboard
section: reference/stdlib
order: 9
---

**Status: Implemented** -- C runtime backing.

The `std.dashboard` module provides a high-level API for building live dashboards. Create a dashboard, add chart and table widgets, push updates, and serve the result as a self-contained web application with WebSocket-based live refresh.

## Types

### $dashboard

Opaque handle to a dashboard instance.

### $dashwidget

| Field | Type | Meaning |
|-------|------|---------|
| id | $str | Widget identifier |
| type | $str | Widget type (`"chart"` or `"table"`) |
| spec | @($byte) | Serialized widget specification |

### $dasherr

| Field | Type | Meaning |
|-------|------|---------|
| msg | $str | Error description |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `dashboard.new` | `title: $str; cols: u64` | `$dashboard` | Create a dashboard with a title and column count |
| `dashboard.addchart` | `d: $dashboard; id: $str; spec: chart.$chartspec` | `$dashwidget` | Add a chart widget |
| `dashboard.addtable` | `d: $dashboard; id: $str; headers: @($str)` | `$dashwidget` | Add a table widget with column headers |
| `dashboard.update` | `d: $dashboard; w: $dashwidget; data: @($byte)` | `void` | Push new data to a widget (triggers live refresh) |
| `dashboard.serve` | `d: $dashboard` | `void!$dasherr` | Start serving the dashboard |

## Usage

```toke
m=example;
i=dashboard:std.dashboard;
i=chart:std.chart;

f=main():i64{
  let d = dashboard.new("Metrics"; 2);
  let c = chart.bar(@("Mon"; "Tue"; "Wed"); @(10.0; 20.0; 15.0); "Daily Hits");
  let w = dashboard.addchart(d; "hits"; c);
  let res = dashboard.serve(d);
  <0;
}
```

## Dependencies

- `std.chart` -- chart specifications for widgets.
- `std.html` -- HTML document construction.
- `std.ws` -- WebSocket-based live updates.
- `std.router` -- HTTP serving.
