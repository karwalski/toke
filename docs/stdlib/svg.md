---
title: std.svg
slug: svg
section: reference/stdlib
order: 34
---

**Status: Implemented** -- C runtime backing.

The `std.svg` module provides a builder API for constructing SVG documents. Create shapes (rectangles, circles, lines, polygons), text labels, and path elements, style them, compose into groups, and render to an SVG string.

## Types

### $svgdoc

| Field | Type | Meaning |
|-------|------|---------|
| width | f64 | Document width |
| height | f64 | Document height |
| viewbox | $str | SVG viewBox attribute |
| elements | @($svgelem) | Child elements |

### $svgelem

Opaque handle to an SVG element.

### $svgstyle

| Field | Type | Meaning |
|-------|------|---------|
| fill | $str | Fill color |
| stroke | $str | Stroke color |
| stroke_width | f64 | Stroke width |
| opacity | f64 | Element opacity (0.0--1.0) |
| font_size | f64 | Font size for text elements |
| font_family | $str | Font family for text elements |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `svg.doc` | `width: f64; height: f64` | `$svgdoc` | Create a new SVG document |
| `svg.rect` | `x: f64; y: f64; w: f64; h: f64; style: $svgstyle` | `$svgelem` | Create a rectangle |
| `svg.circle` | `cx: f64; cy: f64; r: f64; style: $svgstyle` | `$svgelem` | Create a circle |
| `svg.line` | `x1: f64; y1: f64; x2: f64; y2: f64; style: $svgstyle` | `$svgelem` | Create a line |
| `svg.path` | `d: $str; style: $svgstyle` | `$svgelem` | Create a path from an SVG path data string |
| `svg.text` | `x: f64; y: f64; content: $str; style: $svgstyle` | `$svgelem` | Create a text label |
| `svg.group` | `children: @($svgelem); transform: $str` | `$svgelem` | Group elements with an optional transform |
| `svg.polyline` | `points: @(@(f64)); style: $svgstyle` | `$svgelem` | Create a polyline from point pairs |
| `svg.polygon` | `points: @(@(f64)); style: $svgstyle` | `$svgelem` | Create a closed polygon from point pairs |
| `svg.append` | `doc: $svgdoc; elem: $svgelem` | `$svgdoc` | Append an element to the document |
| `svg.render` | `doc: $svgdoc` | `$str` | Render the document to an SVG string |
| `svg.style` | `fill: $str; stroke: $str; stroke_width: f64` | `$svgstyle` | Create a style with common defaults |
| `svg.arrow` | `x1: f64; y1: f64; x2: f64; y2: f64; style: $svgstyle` | `$svgelem` | Create a line with an arrowhead |

## Usage

```toke
m=example;
i=svg:std.svg;

f=main():i64{
  let s = svg.style("#3366cc"; "none"; 0.0);
  let d = svg.doc(200.0; 200.0);
  let d2 = svg.append(d; svg.rect(10.0; 10.0; 80.0; 60.0; s));
  let d3 = svg.append(d2; svg.circle(150.0; 100.0; 40.0; s));
  let out = svg.render(d3);
  <0;
}
```

## Dependencies

None.
