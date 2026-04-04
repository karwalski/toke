# std.dashboard -- Real-Time Dashboard

## Overview

The `std.dashboard` module composes `std.chart`, `std.html`, `std.ws`, and `std.router` into a self-contained, single-program monitoring dashboard server. A toke program creates a dashboard handle, registers chart and table widgets, and calls `dashboard.serve()` to start serving the dashboard over HTTP. Live data is pushed to connected browsers via Server-Sent Events (SSE) using `dashboard.update()`.

## Types

### dashboard

An opaque handle representing a configured dashboard server instance.

### dashwidget

A widget registered with the dashboard.

| Field | Type | Meaning |
|-------|------|---------|
| id | Str | Widget identifier used to target updates |
| type | Str | Widget type: `"chart"` or `"table"` |
| spec | @byte | Serialised widget specification (Chart.js JSON for charts) |

### dasherr

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Description of the serve failure |

## Functions

### dashboard.new(title: Str; port: u64): dashboard

Creates a dashboard handle. The dashboard listens on `0.0.0.0:port`. Call `dashboard.serve()` to begin accepting connections.

**Example:**
```toke
let d = dashboard.new("System Monitor"; 8080);
```

### dashboard.addchart(d: dashboard; id: Str; spec: chartspec): dashwidget

Registers a chart widget. `id` must be unique within the dashboard. `spec` is a `std.chart` chart specification.

**Example:**
```toke
let spec = chart.bar(@("cpu"; "mem"); @(42.0; 67.0); "Resource Usage");
let w = dashboard.addchart(d; "resources"; spec);
```

### dashboard.addtable(d: dashboard; id: Str; headers: @Str): dashwidget

Registers a table widget with the given column headers. Rows are pushed with `dashboard.update()`.

**Example:**
```toke
let t = dashboard.addtable(d; "requests"; @("Method"; "Path"; "Status"; "Latency"));
```

### dashboard.update(d: dashboard; widget: dashwidget; data: @byte): void

Pushes a data update to all connected browsers for `widget` via SSE. For chart widgets, `data` is a JSON array of new data values. For table widgets, `data` is a JSON array of row strings.

**Example:**
```toke
let row = str.bytes("[\"GET\"; \"/api/v1\"; \"200\"; \"8ms\"]");
dashboard.update(d; t; row);
```

### dashboard.serve(d: dashboard): void!dasherr

Starts the dashboard HTTP server. Blocks until the process exits or a fatal error occurs. The generated page auto-refreshes widgets when data is pushed.

**Example:**
```toke
let result = dashboard.serve(d);
match result {
  ok(_)  { log.info("dashboard stopped") };
  err(e) { log.error(e.msg) };
};
```

## Error Types

### dasherr

| Field | Type | Meaning |
|-------|------|---------|
| msg | Str | Description of the bind or runtime failure |
