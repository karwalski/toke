# std.canvas -- 2D Canvas Drawing

## Overview

The `std.canvas` module provides 2D drawing commands for HTML `<canvas>` elements. Programs build a `canvas` value by chaining drawing functions in a builder pattern; each function accepts and returns a `canvas` so calls compose naturally with `let` rebinding. When drawing is complete, `canvas.to_js()` serialises all accumulated commands to a JavaScript string and `canvas.to_html()` emits a self-contained `<canvas>` element plus `<script>` block ready for embedding. Gaming and 3D (WebGL/WebGPU) are explicitly out of scope.

## Types

### canvas

Accumulates drawing state for one `<canvas>` element.

| Field | Type | Meaning |
|-------|------|---------|
| id | $str | HTML `id` attribute of the element |
| width | u32 | Canvas width in pixels |
| height | u32 | Canvas height in pixels |
| ops | @canvasop | Ordered list of drawing commands |

### canvasop

An opaque drawing command. Values are created internally by the drawing functions and serialised by `canvas.to_js()`. Programs do not construct `canvasop` directly.

| Field | Type | Meaning |
|-------|------|---------|
| tag | $str | Command identifier (e.g., `"fillRect"`, `"arc"`) |
| data | $str | Serialised parameters for the command |

## Functions

### canvas.new(id: $str; width: u32; height: u32): canvas

Creates a new canvas with the given HTML element `id` and pixel dimensions. The canvas starts with an empty command list.

**Example:**
```toke
let c = canvas.new("myCanvas"; 800; 600);
```

### canvas.fill_rect(c: canvas; x: f64; y: f64; w: f64; h: f64; color: $str): canvas

Fills a rectangle at (`x`, `y`) with size `w` x `h` using the given CSS `color`.

**Example:**
```toke
let c = canvas.new("bg"; 400; 300);
let c = canvas.fill_rect(c; 0.0; 0.0; 400.0; 300.0; "#1a1a2e");
```

### canvas.stroke_rect(c: canvas; x: f64; y: f64; w: f64; h: f64; color: $str; line_width: f64): canvas

Strokes (outlines) a rectangle with the given CSS `color` and `line_width` in pixels.

**Example:**
```toke
let c = canvas.stroke_rect(c; 10.0; 10.0; 200.0; 100.0; "#ffffff"; 2.0);
```

### canvas.clear_rect(c: canvas; x: f64; y: f64; w: f64; h: f64): canvas

Clears the rectangular region to transparent black, equivalent to the browser `clearRect` call.

**Example:**
```toke
let c = canvas.clear_rect(c; 0.0; 0.0; 800.0; 600.0);
```

### canvas.fill_text(c: canvas; text: $str; x: f64; y: f64; font: $str; color: $str): canvas

Draws filled `text` at position (`x`, `y`) using the given CSS `font` string and `color`. The `font` parameter follows the CSS `font` shorthand syntax (e.g., `"bold 24px sans-serif"`).

**Example:**
```toke
let c = canvas.fill_text(c; "Score: 42"; 20.0; 40.0; "bold 24px sans-serif"; "#f0f0f0");
let c = canvas.fill_text(c; "Game Over"; 150.0; 200.0; "italic 48px serif"; "#ff0000");
```

### canvas.begin_path(c: canvas): canvas

Begins a new path, clearing any previously recorded sub-paths. Use before a series of `move_to`, `line_to`, or `arc` calls.

**Example:**
```toke
let c = canvas.begin_path(c);
```

### canvas.move_to(c: canvas; x: f64; y: f64): canvas

Moves the path pen to (`x`, `y`) without drawing a line.

**Example:**
```toke
let c = canvas.begin_path(c);
let c = canvas.move_to(c; 50.0; 50.0);
```

### canvas.line_to(c: canvas; x: f64; y: f64): canvas

Adds a straight line from the current pen position to (`x`, `y`).

**Example:**
```toke
let c = canvas.begin_path(c);
let c = canvas.move_to(c; 50.0; 50.0);
let c = canvas.line_to(c; 200.0; 150.0);
let c = canvas.stroke(c; "#ff6b6b"; 3.0);
```

### canvas.arc(c: canvas; x: f64; y: f64; r: f64; start_angle: f64; end_angle: f64): canvas

Adds a circular arc centred at (`x`, `y`) with radius `r`. Angles are in radians; `0.0` is the positive x-axis, increasing clockwise. Use `start_angle = 0.0` and `end_angle = 6.283185307` (2*pi) for a full circle.

**Example:**
```toke
(* Full circle centred at 100,100 with radius 40 *)
let pi = 3.141592653589793;
let c = canvas.begin_path(c);
let c = canvas.arc(c; 100.0; 100.0; 40.0; 0.0; pi * 2.0);
let c = canvas.fill(c; "#4ecdc4");
```

### canvas.close_path(c: canvas): canvas

Closes the current sub-path by drawing a straight line back to its start point. Useful for closing polygons before filling or stroking.

**Example:**
```toke
(* Draw a triangle *)
let c = canvas.begin_path(c);
let c = canvas.move_to(c; 100.0; 10.0);
let c = canvas.line_to(c; 190.0; 170.0);
let c = canvas.line_to(c; 10.0; 170.0);
let c = canvas.close_path(c);
let c = canvas.fill(c; "#e74c3c");
```

### canvas.fill(c: canvas; color: $str): canvas

Fills the current path with the given CSS `color`. Must be preceded by path commands (`begin_path`, `move_to`, `line_to`, `arc`, etc.).

**Example:**
```toke
let c = canvas.begin_path(c);
let c = canvas.arc(c; 200.0; 200.0; 50.0; 0.0; 6.283185307);
let c = canvas.fill(c; "rgba(52, 152, 219, 0.8)");
```

### canvas.stroke(c: canvas; color: $str; line_width: f64): canvas

Strokes the current path with the given CSS `color` and `line_width` in pixels.

**Example:**
```toke
let c = canvas.begin_path(c);
let c = canvas.move_to(c; 0.0; 0.0);
let c = canvas.line_to(c; 300.0; 300.0);
let c = canvas.stroke(c; "#2ecc71"; 2.0);
```

### canvas.draw_image(c: canvas; src: $str; x: f64; y: f64; w: f64; h: f64): canvas

Draws an image at (`x`, `y`) scaled to `w` x `h` pixels. `src` may be a URL or a data URI (e.g., `"data:image/png;base64,..."`).

**Example:**
```toke
let c = canvas.draw_image(c; "https://example.com/logo.png"; 10.0; 10.0; 64.0; 64.0);
```

### canvas.set_alpha(c: canvas; alpha: f64): canvas

Sets the global alpha (opacity) for subsequent drawing commands. `alpha` must be in the range `0.0` (fully transparent) to `1.0` (fully opaque). Remember to reset to `1.0` after translucent drawing.

**Example:**
```toke
let c = canvas.set_alpha(c; 0.5);
let c = canvas.fill_rect(c; 0.0; 0.0; 100.0; 100.0; "#000000");
let c = canvas.set_alpha(c; 1.0);  (* restore full opacity *)
```

### canvas.to_js(c: canvas): $str

Serialises all accumulated drawing commands to a JavaScript string. The output declares a variable for the canvas element and its 2D rendering context, then emits each drawing call in order. Suitable for embedding in a `<script>` block alongside a matching `<canvas>` element.

**Example:**
```toke
let js = canvas.to_js(c);
(* js begins with: var _c = document.getElementById("myCanvas"); *)
(*                 var _ctx = _c.getContext("2d");               *)
(*                 _ctx.fillStyle = "#1a1a2e"; ...               *)
```

### canvas.to_html(c: canvas): $str

Emits a complete, self-contained HTML fragment: a `<canvas id=... width=... height=...>` element followed by a `<script>` block containing the serialised drawing commands. Can be written directly to a file or embedded in a `std.html` document via `html.script()`.

**Example:**
```toke
let frag = canvas.to_html(c);
file.write("/tmp/drawing.html"; frag);
```

## Builder Pattern

All drawing functions accept and return a `canvas` value, so calls chain naturally using `let` rebinding. This is the idiomatic way to build up complex drawings:

```toke
let c = canvas.new("diagram"; 500; 500);
let c = canvas.fill_rect(c; 0.0; 0.0; 500.0; 500.0; "#f8f9fa");
let c = canvas.begin_path(c);
let c = canvas.arc(c; 250.0; 250.0; 100.0; 0.0; 6.283185307);
let c = canvas.fill(c; "#4dabf7");
let c = canvas.fill_text(c; "Circle"; 210.0; 255.0; "bold 18px sans-serif"; "#ffffff");
let html = canvas.to_html(c);
```

## Integration with std.html

Use `canvas.to_js()` with the `std.html` document builder to embed a canvas in a full HTML page:

```toke
(* Draw a bar chart *)
let c = canvas.new("chart"; 600; 400);
let c = canvas.fill_rect(c; 0.0; 0.0; 600.0; 400.0; "#ffffff");
let c = canvas.fill_rect(c; 50.0; 300.0; 80.0; -120.0; "#3498db");
let c = canvas.fill_rect(c; 180.0; 300.0; 80.0; -200.0; "#2ecc71");
let c = canvas.fill_rect(c; 310.0; 300.0; 80.0; -160.0; "#e74c3c");
let c = canvas.fill_text(c; "Q1"; 70.0; 330.0; "14px sans-serif"; "#333");
let c = canvas.fill_text(c; "Q2"; 200.0; 330.0; "14px sans-serif"; "#333");
let c = canvas.fill_text(c; "Q3"; 330.0; 330.0; "14px sans-serif"; "#333");

(* Wrap in a full HTML document *)
let d = html.doc();
html.title(d; "Quarterly Report");
html.style(d; "body { font-family: sans-serif; margin: 2em; }");
html.append(d; html.h1("Revenue by Quarter"));
html.script(d; canvas.to_js(c));
let out = html.render(d);
file.write("/tmp/report.html"; out);
```
