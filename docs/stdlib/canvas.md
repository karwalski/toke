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

One drawing operation, as recorded in `$canvas.ops`.

| Field | Type | Meaning |
|-------|------|---------|
| tag | $str | The Canvas 2D member the operation ends with, e.g. `"fillRect"`, `"fillText"` |
| data | $str | The JavaScript statement the operation adds to `canvas.tojs`'s output |

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
| `canvas.fillrect` | `c: $canvas; x: f64; y: f64; w: f64; h: f64; color: $str` | `$canvas` | Draw a filled rectangle |
| `canvas.strokerect` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; x: f64; y: f64; w: f64; h: f64; color: $str; line_width: f64` | `$canvas` | Draw a stroked rectangle |
| `canvas.clearrect` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; x: f64; y: f64; w: f64; h: f64` | `$canvas` | Clear a rectangular area |
| `canvas.filltext` | `c: $canvas; text: $str; x: f64; y: f64; font: $str; color: $str` | `$canvas` | Draw filled text |
| `canvas.beginpath` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas` | `$canvas` | Begin a new path |
| `canvas.moveto` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; x: f64; y: f64` | `$canvas` | Move the path cursor |
| `canvas.lineto` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; x: f64; y: f64` | `$canvas` | Add a line segment to the path |
| `canvas.arc` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; x: f64; y: f64; r: f64; start: f64; end: f64` | `$canvas` | Add an arc to the path |
| `canvas.closepath` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas` | `$canvas` | Close the current path |
| `canvas.fill` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; color: $str` | `$canvas` | Fill the current path |
| `canvas.stroke` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; color: $str; line_width: f64` | `$canvas` | Stroke the current path |
| `canvas.drawimage` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; src: $str; x: f64; y: f64; w: f64; h: f64` | `$canvas` | Draw an image from a URL or data URI |
| `canvas.setalpha` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas; alpha: f64` | `$canvas` | Set global alpha (0.0--1.0) |
| `canvas.tojs` [quarantined](/docs/reference/tki-quarantine/) | `c: $canvas` | `$str` | Export drawing operations as JavaScript code |
| `canvas.tohtml` | `c: $canvas` | `$str` | Export as a self-contained HTML page |

## Escaping

Colours, fonts, text and the canvas id are written into the generated
JavaScript as escaped string literals (and the id into the `<canvas>` element
as an escaped attribute), so a text such as `it's </script>` is drawn as that
text and cannot end the string or the `<script>` element early.

## Usage

```toke
m=example;
i=canvas:std.canvas;

f=main():i64{
  let c = canvas.new("mycanvas"; 400; 300);
  let c2 = canvas.fillrect(c; 0.0; 0.0; 400.0; 300.0; "#ffffff");
  let c3 = canvas.fillrect(c2; 50.0; 50.0; 100.0; 80.0; "#3366cc");
  let c4 = canvas.filltext(c3; "Hello"; 60.0; 100.0; "24px sans-serif"; "#ffffff");
  let html = canvas.tohtml(c4);
  <0;
}
```

## Dependencies

None.
