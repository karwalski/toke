# std.dashboard

Live dashboard server: compose charts and tables into a self-updating web dashboard with WebSocket push.

## Types

```
type dashboard  { id: u64 }                          -- opaque
type dashwidget { id: str, type: str, spec: [byte] }
type dasherr    { msg: str }
```

## Functions

```
f=new(title:str;port:u64):dashboard
f=addchart(d:dashboard;label:str;spec:chart.chartspec):dashwidget
f=addtable(d:dashboard;label:str;headers:[str]):dashwidget
f=update(d:dashboard;w:dashwidget;data:[byte]):void
f=serve(d:dashboard):void!dasherr
```

## Semantics

- `new` creates a dashboard with a title and HTTP port. Does not start serving until `serve` is called.
- `addchart` adds a chart widget. The `chart.chartspec` defines the initial chart; it can be updated later via `update`.
- `addtable` adds a table widget with the given column headers and no initial rows.
- `update` pushes new data to a widget. The `data` bytes are JSON: for charts, a new `chartspec`; for tables, an array of row arrays. Connected browsers receive the update via WebSocket.
- `serve` starts the HTTP server and blocks. Returns `dasherr` if the port is unavailable.

## Dependencies

- `std.chart` (chart specifications)
- `std.html` (page rendering)
- `std.ws` (live update push)
- `std.router` (HTTP serving)
