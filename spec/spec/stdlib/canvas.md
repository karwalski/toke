# std.canvas

HTML5 Canvas 2D drawing: build a sequence of drawing operations and emit as JavaScript or a self-contained HTML page.

## Types

```
type canvasop { tag: str, data: str }  -- opaque
type canvas   { id: str, width: u32, height: u32, ops: [canvasop] }
```

## Functions

```
f=new(id:str;width:u32;height:u32):canvas
f=fill_rect(c:canvas;x:f64;y:f64;w:f64;h:f64;color:str):canvas
f=stroke_rect(c:canvas;x:f64;y:f64;w:f64;h:f64;color:str;linewidth:f64):canvas
f=clear_rect(c:canvas;x:f64;y:f64;w:f64;h:f64):canvas
f=fill_text(c:canvas;text:str;x:f64;y:f64;font:str;color:str):canvas
f=begin_path(c:canvas):canvas
f=move_to(c:canvas;x:f64;y:f64):canvas
f=line_to(c:canvas;x:f64;y:f64):canvas
f=arc(c:canvas;cx:f64;cy:f64;r:f64;start:f64;end:f64):canvas
f=close_path(c:canvas):canvas
f=fill(c:canvas;color:str):canvas
f=stroke(c:canvas;color:str;linewidth:f64):canvas
f=draw_image(c:canvas;src:str;x:f64;y:f64;w:f64;h:f64):canvas
f=set_alpha(c:canvas;alpha:f64):canvas
f=to_js(c:canvas):str
f=to_html(c:canvas):str
```

## Semantics

- All drawing functions return a new `canvas` with the operation appended (builder pattern).
- `new` creates a canvas with the given HTML `id` attribute and pixel dimensions.
- Path operations (`begin_path`, `move_to`, `line_to`, `arc`, `close_path`) build a path; `fill` / `stroke` render it.
- `arc` angles are in radians.
- `draw_image` references an image by URL or data URI.
- `set_alpha` sets `globalAlpha` for subsequent operations.
- `to_js` emits JavaScript code targeting a `<canvas>` element with the matching `id`.
- `to_html` emits a complete HTML page with an embedded `<canvas>` and the drawing script.

## Dependencies

None.
