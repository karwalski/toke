# std.canvas -- JS Canvas API Bindings

## Overview

The `std.canvas` module provides 2D drawing commands for HTML `<canvas>` elements. Programs build a `canvas` value by chaining drawing functions in a builder pattern; each function returns an updated `canvas` so calls compose naturally. When the canvas is complete, `canvas.to_js()` serialises all accumulated drawing commands to a JavaScript string, and `canvas.to_html()` emits a self-contained `<canvas>` element plus `<script>` block ready for embedding in a `std.html` document via `html.script()`.

Gaming and 3D (WebGL/WebGPU) are explicitly out of scope.

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

## Functions

### canvas.new(id: $str; width: u32; height: u32): canvas

Creates a new canvas with the given HTML element `id` and pixel dimensions. The canvas starts with an empty command list.

**Example:**
```toke
let c = canvas.new("myCanvas"; 800; 600);
```

### canvas.fill_rect(c: canvas; x: f64; y: f64; w: f64; h: f64; color: $str): canvas

Fills a rectangle with the given CSS `color`.

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

Draws filled `text` at position (`x`, `y`) using the given CSS `font` string and `color`.

**Example:**
```toke
let c = canvas.fill_text(c; "Score: 42"; 20.0; 40.0; "bold 24px sans-serif"; "#f0f0f0");
```

### canvas.begin_path(c: canvas): canvas

Begins a new path, clearing any previously recorded sub-paths. Use before a series of `move_to`, `line_to`, or `arc` calls.

**Example:**
```toke
let c = canvas.begin_path(c);
```

### canvas.move_to(c: canvas; x: f64; y: f64): canvas

Moves the path pen to (`x`, `y`) without drawing.

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

Adds a circular arc centred at (`x`, `y`) with radius `r`. Angles are in radians; 0 is the positive x-axis, increasing clockwise.

**Example:**
```toke
(* Full circle centred at 100,100 with radius 40 *)
let pi = 3.141592653589793;
let c = canvas.begin_path(c);
let c = canvas.arc(c; 100.0; 100.0; 40.0; 0.0; pi*2.0);
let c = canvas.fill(c; "#4ecdc4");
```

### canvas.close_path(c: canvas): canvas

Closes the current sub-path by drawing a straight line back to its start point.

### canvas.fill(c: canvas; color: $str): canvas

Fills the current path with the given CSS `color`.

### canvas.stroke(c: canvas; color: $str; line_width: f64): canvas

Strokes the current path with the given CSS `color` and `line_width` in pixels.

### canvas.draw_image(c: canvas; src: $str; x: f64; y: f64; w: f64; h: f64): canvas

Draws an image at (`x`, `y`) scaled to `w` × `h` pixels. `src` may be a URL or a data URI (e.g., `"data:image/png;base64,..."`).

**Example:**
```toke
let c = canvas.draw_image(c; "https://example.com/logo.png"; 10.0; 10.0; 64.0; 64.0);
```

### canvas.set_alpha(c: canvas; alpha: f64): canvas

Sets the global alpha (opacity) for subsequent drawing commands. `alpha` must be in the range `0.0` (fully transparent) to `1.0` (fully opaque).

**Example:**
```toke
let c = canvas.set_alpha(c; 0.5);
let c = canvas.fill_rect(c; 0.0; 0.0; 100.0; 100.0; "#000000");
let c = canvas.set_alpha(c; 1.0);
```

### canvas.to_js(c: canvas): $str

Serialises all accumulated drawing commands to a JavaScript string. The output assumes a variable named `_ctx` holds the 2D rendering context. This is suitable for embedding in a `<script>` block alongside a matching `<canvas>` element.

**Example:**
```toke
let js = canvas.to_js(c);
(* js begins with: var _c = document.getElementById("myCanvas"); *)
(*                 var _ctx = _c.getContext("2d");               *)
(*                 _ctx.fillStyle = "#1a1a2e"; ...               *)
```

### canvas.to_html(c: canvas): $str

Emits a complete, self-contained fragment: a `<canvas id=... width=... height=...>` element followed by a `<script>` block containing the serialised drawing commands. Use with `html.script()` or embed directly in a `std.html` document.

**Example:**
```toke
let c = canvas.new("chart"; 600; 400);
let c = canvas.fill_rect(c; 0.0; 0.0; 600.0; 400.0; "#ffffff");
let c = canvas.fill_text(c; "Hello Canvas"; 50.0; 50.0; "20px sans-serif"; "#333333");

(* Standalone fragment *)
let frag = canvas.to_html(c);

(* Integrate with std.html *)
let d = html.doc();
html.title(d; "Canvas Demo");
html.script(d; canvas.to_js(c));
html.append(d; html.h1("My Drawing"));
let out = html.render(d);
```

## Builder Pattern

All drawing functions accept and return a `canvas` value, so calls chain naturally using `let` rebinding:

```toke
let c = canvas.new("diagram"; 500; 500);
let c = canvas.fill_rect(c; 0.0; 0.0; 500.0; 500.0; "#f8f9fa");
let c = canvas.begin_path(c);
let c = canvas.arc(c; 250.0; 250.0; 100.0; 0.0; 6.283185307);
let c = canvas.fill(c; "#4dabf7");
let c = canvas.fill_text(c; "Circle"; 210.0; 255.0; "bold 18px sans-serif"; "#ffffff");
let html = canvas.to_html(c);
```
