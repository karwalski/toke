# std.dashboard -- Real-Time Dashboard

## Overview

The `std.dashboard` module composes `std.chart`, `std.html`, `std.ws`, and `std.router` into a self-contained monitoring dashboard server. A toke program creates a dashboard handle, registers chart and table widgets, pushes live data updates, and serves the result over HTTP. Live data is pushed to connected browsers via Server-Sent Events (SSE).

Import with:

```toke
i=dash:std.dashboard;
i=chart:std.chart;
```

## Types

### dashboard

An opaque handle representing a configured dashboard server instance. Created by `dashboard.new`. Holds registered widgets and the HTTP listener configuration.

| Field | Type | Description |
|-------|------|-------------|
| `id`  | `u64` | Runtime handle identifier (opaque). |

### dashwidget

A widget registered with the dashboard. Each widget has a unique identifier and a type that determines how data updates are rendered in the browser.

| Field | Type | Description |
|-------|------|-------------|
| `id`   | `$str`   | Widget identifier used to target updates. |
| `type` | `$str`   | Widget type: `"chart"` or `"table"`. |
| `spec` | `@byte`  | Serialised widget specification (Chart.js JSON for charts). |

### dasherr

An error produced when the dashboard server fails to bind or encounters a runtime error.

| Field | Type | Description |
|-------|------|-------------|
| `msg` | `$str` | Human-readable description of the failure. |

## Functions

### dashboard.new(title: $str; port: u64): dashboard

Creates a new dashboard handle. The dashboard will listen on `0.0.0.0:port` when `dashboard.serve` is called. `title` appears in the generated HTML page header.

**Example:**
```toke
let d = dashboard.new("System Monitor"; 8080);
```

### dashboard.addchart(d: dashboard; id: $str; spec: chartspec): dashwidget

Registers a chart widget with the dashboard. `id` must be unique within the dashboard. `spec` is a `std.chart` chart specification that defines the chart type, labels, and initial data.

Returns a `dashwidget` handle used to push data updates with `dashboard.update`.

**Example:**
```toke
let spec = chart.bar(@("cpu"; "mem"); @(42.0; 67.0); "Resource Usage");
let w = dashboard.addchart(d; "resources"; spec);
```

### dashboard.addtable(d: dashboard; id: $str; headers: @$str): dashwidget

Registers a table widget with the given column headers. `id` must be unique within the dashboard. Rows are pushed with `dashboard.update` after registration.

Returns a `dashwidget` handle used to push row data.

**Example:**
```toke
let t = dashboard.addtable(d; "requests"; @("Method"; "Path"; "Status"; "Latency"));
```

### dashboard.update(d: dashboard; widget: dashwidget; data: @byte): void

Pushes a data update to all connected browsers for `widget` via SSE. The format of `data` depends on the widget type:

- **Chart widgets:** `data` is a JSON array of new data values that replace the current dataset.
- **Table widgets:** `data` is a JSON array of row strings appended to the table.

Updates are delivered in real time to all active browser connections.

**Example:**
```toke
(* Update a chart with new values *)
let vals = str.bytes("[55.0; 72.0]");
dashboard.update(d; w; vals);

(* Append a row to a table *)
let row = str.bytes("[\"GET\"; \"/api/v1\"; \"200\"; \"8ms\"]");
dashboard.update(d; t; row);
```

### dashboard.serve(d: dashboard): void!dasherr

Starts the dashboard HTTP server. Blocks until the process exits or a fatal error occurs. The generated page includes all registered widgets and auto-refreshes them when data is pushed via `dashboard.update`.

Returns `dasherr` if the server cannot bind to the configured port or encounters a fatal runtime error.

**Example:**
```toke
let result = dashboard.serve(d);
match result {
  ok(_)  { log.info("dashboard stopped") };
  err(e) { log.error(e.msg) };
};
```

## Usage Example

Building a metrics dashboard with a bar chart and a request log table:

```toke
i=dash:std.dashboard;
i=chart:std.chart;
i=log:std.log;
i=str:std.str;
i=time:std.time;

M=monitor;

F=main(): void!dasherr {
  (* Create dashboard on port 3000 *)
  let d = dashboard.new("App Monitor"; 3000);

  (* Add a bar chart for system resources *)
  let cpu_spec = chart.bar(
    @("CPU"; "Memory"; "Disk");
    @(0.0; 0.0; 0.0);
    "System Resources (%)"
  );
  let cpu_widget = dashboard.addchart(d; "sys_resources"; cpu_spec);

  (* Add a table for recent HTTP requests *)
  let req_table = dashboard.addtable(
    d;
    "http_log";
    @("Time"; "Method"; "Path"; "Status"; "Latency")
  );

  (* Start polling system metrics in background *)
  spawn {
    loop {
      let cpu = sys.cpu_percent();
      let mem = sys.mem_percent();
      let disk = sys.disk_percent();
      let data = str.bytes(
        str.concat("["; str.concat(
          str.from_float(cpu); str.concat("; "; str.concat(
            str.from_float(mem); str.concat("; "; str.concat(
              str.from_float(disk); "]"
            ))
          ))
        ))
      );
      dashboard.update(d; cpu_widget; data);
      time.sleep(2000);
    };
  };

  (* Start the server -- blocks until exit *)
  <dashboard.serve(d)
};
```

Adding a request to the table from an HTTP handler:

```toke
F=log_request(d: dashboard; widget: dashwidget; method: $str; path: $str; status: $str; latency: $str): void {
  let now = time.now_iso();
  let row = str.bytes(
    str.concat("[\""; str.concat(now; str.concat("\"; \""; str.concat(
      method; str.concat("\"; \""; str.concat(
        path; str.concat("\"; \""; str.concat(
          status; str.concat("\"; \""; str.concat(
            latency; "\"]"
          ))
        ))
      ))
    ))))
  );
  dashboard.update(d; widget; row);
};
```

## Notes

- Register all widgets before calling `dashboard.serve`. Adding widgets after the server starts is not supported.
- `dashboard.serve` blocks the calling thread. Use `spawn` to run background data collection alongside the server.
- Widget `id` values must be unique within a single dashboard instance. Duplicate IDs produce undefined behaviour.
- The generated HTML page uses Chart.js for chart rendering and plain HTML tables for table widgets.
- SSE connections auto-reconnect if the browser loses connectivity.
- For custom page layouts beyond the default grid, use `std.html` and `std.router` directly instead of `std.dashboard`.
