---
title: std.canvas
slug: canvas
section: reference/stdlib
order: 4
---

**Status: Implemented** -- C runtime backing.

The `std.canvas` module provides a chainable API for constructing HTML5 Canvas drawing instructions. Build up drawing operations (rectangles, paths, text, images), then export as JavaScript code or a self-contained HTML page.

## Types

### $canvasop

Opaque handle to a single canvas drawing operation.

### $canvas

| Field | Type | Meaning |
|-------|------|---------|
| id | $str | Canvas element ID |
| width | u32 | Canvas width in pixels |
| height | u32 | Canvas height in pixels |
| ops | @($canvasop) | Accumulated drawing operations |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `canvas.new` | `id: $str; w: u32; h: u32` | `$canvas` | Create a new canvas |
| `canvas.fill_rect` | `c: $canvas; x: f64; y: f64; w: f64; h: f64; color: $str` | `$canvas` | Draw a filled rectangle |
| `canvas.stroke_rect` | `c: $canvas; x: f64; y: f64; w: f64; h: f64; color: $str; line_width: f64` | `$canvas` | Draw a stroked rectangle |
| `canvas.clear_rect` | `c: $canvas; x: f64; y: f64; w: f64; h: f64` | `$canvas` | Clear a rectangular area |
| `canvas.fill_text` | `c: $canvas; text: $str; x: f64; y: f64; font: $str; color: $str` | `$canvas` | Draw filled text |
| `canvas.begin_path` | `c: $canvas` | `$canvas` | Begin a new path |
| `canvas.move_to` | `c: $canvas; x: f64; y: f64` | `$canvas` | Move the path cursor |
| `canvas.line_to` | `c: $canvas; x: f64; y: f64` | `$canvas` | Add a line segment to the path |
| `canvas.arc` | `c: $canvas; x: f64; y: f64; r: f64; start: f64; end: f64` | `$canvas` | Add an arc to the path |
| `canvas.close_path` | `c: $canvas` | `$canvas` | Close the current path |
| `canvas.fill` | `c: $canvas; color: $str` | `$canvas` | Fill the current path |
| `canvas.stroke` | `c: $canvas; color: $str; line_width: f64` | `$canvas` | Stroke the current path |
| `canvas.draw_image` | `c: $canvas; src: $str; x: f64; y: f64; w: f64; h: f64` | `$canvas` | Draw an image from a URL or data URI |
| `canvas.set_alpha` | `c: $canvas; alpha: f64` | `$canvas` | Set global alpha (0.0--1.0) |
| `canvas.to_js` | `c: $canvas` | `$str` | Export drawing operations as JavaScript code |
| `canvas.to_html` | `c: $canvas` | `$str` | Export as a self-contained HTML page |

## Usage

```toke
m=example;
i=canvas:std.canvas;

f=main():i64{
  let c = canvas.new("mycanvas"; 400; 300);
  let c2 = canvas.fill_rect(c; 0.0; 0.0; 400.0; 300.0; "#ffffff");
  let c3 = canvas.fill_rect(c2; 50.0; 50.0; 100.0; 80.0; "#3366cc");
  let c4 = canvas.fill_text(c3; "Hello"; 60.0; 100.0; "24px sans-serif"; "#ffffff");
  let html = canvas.to_html(c4);
  <0;
}
```

## Dependencies

None.
