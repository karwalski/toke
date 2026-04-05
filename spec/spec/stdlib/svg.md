# std.svg

SVG document construction: shapes, paths, text, grouping, styling, and rendering to string.

## Types

```
type svgdoc   { width: f64, height: f64, viewbox: str, elements: [svgelem] }
type svgelem  -- opaque
type svgstyle { fill: str, stroke: str, stroke_width: f64, opacity: f64, font_size: f64, font_family: str }
```

## Functions

```
f=doc(width:f64;height:f64):svgdoc
f=rect(x:f64;y:f64;w:f64;h:f64;style:svgstyle):svgelem
f=circle(cx:f64;cy:f64;r:f64;style:svgstyle):svgelem
f=line(x1:f64;y1:f64;x2:f64;y2:f64;style:svgstyle):svgelem
f=path(d:str;style:svgstyle):svgelem
f=text(x:f64;y:f64;content:str;style:svgstyle):svgelem
f=group(elems:[svgelem];transform:str):svgelem
f=polyline(points:[[f64]];style:svgstyle):svgelem
f=polygon(points:[[f64]];style:svgstyle):svgelem
f=append(doc:svgdoc;elem:svgelem):svgdoc
f=render(doc:svgdoc):str
f=style(fill:str;stroke:str;stroke_width:f64):svgstyle
f=arrow(x1:f64;y1:f64;x2:f64;y2:f64;style:svgstyle):svgelem
```

## Semantics

- `doc` creates an SVG document with the given width/height. `viewbox` defaults to `"0 0 width height"`.
- Shape functions (`rect`, `circle`, `line`, `path`, `text`) create SVG elements with the given style.
- `path` accepts an SVG path `d` attribute string (e.g., `"M 10 10 L 90 90"`).
- `group` wraps elements in a `<g>` with an optional SVG `transform` attribute.
- `polyline` / `polygon` take points as `[[x, y], ...]` pairs.
- `append` returns a new `svgdoc` with the element added (immutable update).
- `render` serializes the document to an SVG XML string.
- `style` is a convenience constructor; unset fields default to empty string / 0.0.
- `arrow` draws a line with an arrowhead marker at the endpoint.

## Dependencies

None.
