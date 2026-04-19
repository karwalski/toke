# std.svg -- Programmatic SVG Generation

## Overview

The `std.svg` module provides a structured API for building SVG documents in toke programs. Functions construct `svgelem` values that are appended to an `svgdoc` handle. `svg.render()` emits a complete, self-contained SVG string suitable for embedding in HTML or writing to a `.svg` file.

Typical use cases include custom diagrams, flowcharts, node graphs, and data visualisations where `std.chart` does not provide enough layout control.

## Types

### svgdoc

The SVG document root. Created by `svg.doc()`.

| Field | Type | Meaning |
|-------|------|---------|
| width | $f64 | Canvas width in user units |
| height | $f64 | Canvas height in user units |
| viewbox | $str | Raw `viewBox` attribute value (e.g., `"0 0 800 600"`) |
| elements | @(svgelem) | Top-level elements in document order |

### svgelem

An opaque handle representing a single SVG element or group. Created by the shape and group constructors; consumed by `svg.append` and `svg.group`. You cannot inspect or modify an `svgelem` after creation -- build a new one instead.

### svgstyle

Presentation attributes applied to a shape.

| Field | Type | Meaning |
|-------|------|---------|
| fill | $str | Fill colour (CSS colour string or `"none"`) |
| stroke | $str | Stroke colour |
| stroke_width | $f64 | Stroke width in user units |
| opacity | $f64 | Overall opacity (0.0--1.0; 1.0 = fully opaque) |
| font_size | $f64 | Font size for text elements (user units) |
| font_family | $str | Font family for text elements |

## Functions

### svg.doc(width: $f64; height: $f64): svgdoc

Creates an empty SVG document with the given dimensions. The `viewbox` is initialised to `"0 0 <width> <height>"` and can be overridden by writing the field directly.

**Example:**
```toke
let d = svg.doc(800.0; 600.0);
```

### svg.style(fill: $str; stroke: $str; stroke_width: $f64): svgstyle

Convenience constructor for the most common style fields. `opacity` defaults to `1.0`; `font_size` and `font_family` default to `0.0` and `""` respectively (the renderer omits them when zero/empty). For full control, construct an `svgstyle` struct literal instead.

**Example:**
```toke
let s = svg.style("steelblue"; "navy"; 1.5);

(* equivalent struct literal with all fields *)
let s2 = $svgstyle{
    fill: "steelblue";
    stroke: "navy";
    stroke_width: 1.5;
    opacity: 1.0;
    font_size: 0.0;
    font_family: ""
};
```

### svg.rect(x: $f64; y: $f64; width: $f64; height: $f64; style: svgstyle): svgelem

Creates a `<rect>` element at `(x, y)` with the given dimensions and style. The coordinates refer to the top-left corner.

**Example:**
```toke
let s = svg.style("gold"; "black"; 1.0);
let r = svg.rect(50.0; 50.0; 200.0; 100.0; s);
```

### svg.circle(cx: $f64; cy: $f64; r: $f64; style: svgstyle): svgelem

Creates a `<circle>` element centred at `(cx, cy)` with radius `r`.

**Example:**
```toke
let s = svg.style("tomato"; "darkred"; 2.0);
let c = svg.circle(400.0; 300.0; 50.0; s);
```

### svg.line(x1: $f64; y1: $f64; x2: $f64; y2: $f64; style: svgstyle): svgelem

Creates a `<line>` element from `(x1, y1)` to `(x2, y2)`. Set `fill` to `"none"` since lines have no fill area.

**Example:**
```toke
let s = svg.style("none"; "grey"; 1.0);
let l = svg.line(0.0; 0.0; 800.0; 600.0; s);
```

### svg.text(x: $f64; y: $f64; content: $str; style: svgstyle): svgelem

Creates a `<text>` element at `(x, y)` with the given string content. Set `font_size` and `font_family` on the style to control typography. The `(x, y)` coordinate is the text baseline anchor point.

**Example:**
```toke
let s = $svgstyle{
    fill: "#333";
    stroke: "none";
    stroke_width: 0.0;
    opacity: 1.0;
    font_size: 16.0;
    font_family: "sans-serif"
};
let t = svg.text(100.0; 80.0; "Hello, toke!"; s);
```

### svg.path(d: $str; style: svgstyle): svgelem

Creates a `<path>` element using the SVG path data string `d`. All standard SVG path commands (`M`, `L`, `C`, `Q`, `A`, `Z`, etc.) are accepted verbatim. This is the most flexible shape primitive.

**Example:**
```toke
let s = svg.style("none"; "black"; 1.5);
let p = svg.path("M 100 200 C 200 100 300 300 400 200"; s);
```

### svg.polyline(points: @(@f64); style: svgstyle): svgelem

Creates a `<polyline>` from a list of `[x, y]` coordinate pairs. The shape is open (first and last points are not joined). Each inner array must contain exactly two elements.

**Example:**
```toke
let pts = @(@(0.0; 0.0); @(100.0; 50.0); @(200.0; 20.0); @(300.0; 80.0));
let s = svg.style("none"; "steelblue"; 2.0);
let pl = svg.polyline(pts; s);
```

### svg.polygon(points: @(@f64); style: svgstyle): svgelem

Creates a `<polygon>` from a list of `[x, y]` coordinate pairs. The shape is automatically closed (the last point connects back to the first).

**Example:**
```toke
let pts = @(@(150.0; 10.0); @(250.0; 190.0); @(50.0; 190.0));
let s = svg.style("limegreen"; "darkgreen"; 1.5);
let tri = svg.polygon(pts; s);
```

### svg.arrow(x1: $f64; y1: $f64; x2: $f64; y2: $f64; style: svgstyle): svgelem

Creates a line from `(x1, y1)` to `(x2, y2)` with an arrowhead marker at the endpoint. The arrowhead is sized proportionally to `stroke_width`. Useful for flowcharts and directed graphs.

**Example:**
```toke
let s = svg.style("none"; "#555"; 2.0);
let a = svg.arrow(100.0; 100.0; 300.0; 100.0; s);
```

### svg.group(elems: @(svgelem); transform: $str): svgelem

Creates a `<g>` group containing `elems`. Pass `transform` as a standard SVG transform string (e.g., `"translate(10,20)"`, `"rotate(45)"`); pass `""` to omit the attribute. Groups are useful for positioning a set of shapes together.

**Example:**
```toke
let s = svg.style("cornflowerblue"; "none"; 0.0);
let r1 = svg.rect(0.0; 0.0; 40.0; 40.0; s);
let r2 = svg.rect(50.0; 0.0; 40.0; 40.0; s);
let g = svg.group(@(r1; r2); "translate(100,100)");
```

### svg.append(doc: svgdoc; elem: svgelem): svgdoc

Adds `elem` to the top-level element list of `doc` and returns the updated document. The original `doc` value should be replaced with the return value since `svgdoc` is immutable.

**Example:**
```toke
let d = svg.doc(400.0; 300.0);
let s = svg.style("coral"; "black"; 1.0);
let d = svg.append(d; svg.rect(10.0; 10.0; 80.0; 60.0; s));
let d = svg.append(d; svg.circle(200.0; 150.0; 40.0; s));
```

### svg.render(doc: svgdoc): $str

Emits the SVG document as a complete XML string, including the `<svg>` root element with `xmlns`, `width`, `height`, and `viewBox` attributes. The returned string can be written to a file, embedded in HTML, or served over HTTP.

**Example:**
```toke
let xml = svg.render(d);
file.write("/tmp/diagram.svg"; xml);
```

## Full Example -- Building a Flowchart

The following program builds a three-box flowchart with arrows connecting each stage, then writes the result to a file.

```toke
m=flowchart;

f=main(): i64 {
    let d = svg.doc(600.0; 400.0);

    (* define reusable styles *)
    let box_style = svg.style("#e8f4f8"; "#2c7bb6"; 2.0);
    let txt_style = $svgstyle{
        fill: "#1a1a1a";
        stroke: "none";
        stroke_width: 0.0;
        opacity: 1.0;
        font_size: 14.0;
        font_family: "sans-serif"
    };
    let arr_style = svg.style("none"; "#555"; 1.5);

    (* row 1: Start *)
    let d = svg.append(d; svg.rect(50.0; 60.0; 160.0; 60.0; box_style));
    let d = svg.append(d; svg.text(130.0; 95.0; "Start"; txt_style));

    (* arrow down *)
    let d = svg.append(d; svg.arrow(130.0; 120.0; 130.0; 180.0; arr_style));

    (* row 2: Process *)
    let d = svg.append(d; svg.rect(50.0; 180.0; 160.0; 60.0; box_style));
    let d = svg.append(d; svg.text(130.0; 215.0; "Process"; txt_style));

    (* arrow down *)
    let d = svg.append(d; svg.arrow(130.0; 240.0; 130.0; 300.0; arr_style));

    (* row 3: End *)
    let d = svg.append(d; svg.rect(50.0; 300.0; 160.0; 60.0; box_style));
    let d = svg.append(d; svg.text(130.0; 335.0; "End"; txt_style));

    (* render and write to file *)
    let out = svg.render(d);
    (file.write("/tmp/flowchart.svg"; out));
    < 0
};
```

## Quick-Reference Table

| Function | Returns | Purpose |
|----------|---------|---------|
| `svg.doc` | `svgdoc` | Create empty document |
| `svg.style` | `svgstyle` | Shorthand style constructor |
| `svg.rect` | `svgelem` | Rectangle |
| `svg.circle` | `svgelem` | Circle |
| `svg.line` | `svgelem` | Line segment |
| `svg.text` | `svgelem` | Text label |
| `svg.path` | `svgelem` | Arbitrary SVG path |
| `svg.polyline` | `svgelem` | Open polyline |
| `svg.polygon` | `svgelem` | Closed polygon |
| `svg.arrow` | `svgelem` | Line with arrowhead |
| `svg.group` | `svgelem` | Group with transform |
| `svg.append` | `svgdoc` | Add element to document |
| `svg.render` | `$str` | Emit SVG XML string |

## See Also

- `std.chart` -- for high-level data charts (bar, line, scatter, pie)
- `std.html` -- for embedding SVG in web pages
- `std.file` -- for writing rendered SVG to disk
