# Cookbook: Graphics Pipeline

## Overview

Generate an SVG diagram programmatically, render it to a PNG image, and save both files to disk. Combines `std.image`, `std.svg`, and `std.file` to demonstrate the graphics generation pipeline in toke.

## Modules Used

- `std.image` -- Image creation, rendering, and format conversion
- `std.svg` -- SVG element construction and serialization
- `std.file` -- File system read and write

## Complete Program

```toke
I std.image;
I std.svg;
I std.file;
I std.str;

(* Create an SVG canvas *)
let doc = svg.new(svg.Canvas{
    width: 600;
    height: 400;
    background: "#ffffff";
});

(* Draw a title *)
svg.add(doc; svg.text(svg.TextConfig{
    x: 300; y: 40;
    content: "System Architecture";
    font_size: 24;
    fill: "#1e293b";
    anchor: svg.Middle;
}));

(* Draw three boxes representing components *)
let box_y = 100;
let box_w = 140;
let box_h = 60;
let colors = @("#3b82f6"; "#10b981"; "#f59e0b");
let labels = @("API Server"; "Database"; "Cache");
let x_positions = @(80; 230; 380);

let i = 0;
for label in labels {
    let x = x_positions.(i);
    let color = colors.(i);

    (* Draw the rounded rectangle *)
    svg.add(doc; svg.rect(svg.RectConfig{
        x: x; y: box_y;
        width: box_w; height: box_h;
        rx: 8; ry: 8;
        fill: color;
        stroke: "#334155";
        stroke_width: 2;
    }));

    (* Draw the label centered in the box *)
    svg.add(doc; svg.text(svg.TextConfig{
        x: x + box_w / 2; y: box_y + box_h / 2 + 6;
        content: label;
        font_size: 14;
        fill: "#ffffff";
        anchor: svg.Middle;
    }));

    i = i + 1;
};

(* Draw arrows between the boxes *)
fn draw_arrow(doc: svg.Doc; x1: i64; y1: i64; x2: i64; y2: i64) {
    svg.add(doc; svg.line(svg.LineConfig{
        x1: x1; y1: y1;
        x2: x2; y2: y2;
        stroke: "#64748b";
        stroke_width: 2;
        marker_end: svg.ArrowHead;
    }));
};

(* API Server -> Database *)
draw_arrow(doc; 220; 130; 230; 130);

(* API Server -> Cache *)
draw_arrow(doc; 220; 120; 380; 120);

(* Draw a legend at the bottom *)
let legend_y = 220;
svg.add(doc; svg.text(svg.TextConfig{
    x: 80; y: legend_y;
    content: "Data flow between components";
    font_size: 12;
    fill: "#94a3b8";
    anchor: svg.Start;
}));

(* Add a border around the entire diagram *)
svg.add(doc; svg.rect(svg.RectConfig{
    x: 10; y: 10;
    width: 580; height: 380;
    rx: 4; ry: 4;
    fill: "none";
    stroke: "#e2e8f0";
    stroke_width: 1;
}));

(* Serialize the SVG to a string *)
let svg_str = svg.render(doc);

(* Write the SVG file *)
file.write_text("output/architecture.svg"; svg_str)!;

(* Render the SVG to a PNG image *)
let img = image.from_svg(doc; image.RenderConfig{
    width: 1200;
    height: 800;
    scale: 2.0;
});

(* Encode as PNG bytes and write to disk *)
let png_bytes = image.encode_png(img)!;
file.write_bytes("output/architecture.png"; png_bytes)!;

(* Also generate a thumbnail *)
let thumb = image.resize(img; 300; 200; image.Lanczos);
let thumb_bytes = image.encode_png(thumb)!;
file.write_bytes("output/architecture_thumb.png"; thumb_bytes)!;
```

## Step-by-Step Explanation

1. **SVG canvas** -- `svg.new` creates a document with specified dimensions and background color. All elements are added to this document.

2. **Text elements** -- `svg.text` creates positioned text with font size, color, and alignment. The `anchor: svg.Middle` centers text horizontally at the given x coordinate.

3. **Rectangles** -- `svg.rect` draws rounded rectangles. The `rx` and `ry` properties control corner rounding. Labels are overlaid by positioning text at the center of each box.

4. **Arrows** -- `svg.line` draws connecting lines between components. The `marker_end: svg.ArrowHead` adds an arrowhead at the endpoint.

5. **SVG output** -- `svg.render` serializes the entire document to an SVG XML string. `file.write_text` saves it to disk.

6. **PNG rendering** -- `image.from_svg` rasterizes the SVG at the specified resolution. A 2x scale produces a crisp retina-ready image.

7. **Thumbnail** -- `image.resize` scales the image down using Lanczos interpolation for high-quality downsampling. Both the full-size and thumbnail PNGs are written to disk.
