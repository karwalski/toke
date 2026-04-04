# std.image -- Image Decode, Transform, and Encode

## Overview

The `std.image` module provides pixel-buffer operations for image processing in API handlers and data pipelines. It supports decoding PNG, JPEG, and WebP bytes into an in-memory pixel buffer (`imgbuf`), geometric transforms (resize, crop, flip), channel conversion, and re-encoding to any supported format. The implementation has no external runtime dependencies beyond libc. Decode and encode operations return `!str` on failure; purely geometric transforms are infallible.

## Types

### imgbuf

An in-memory pixel buffer.

| Field | Type | Meaning |
|-------|------|---------|
| width | u32 | Width in pixels |
| height | u32 | Height in pixels |
| channels | u8 | Bytes per pixel (1 = grayscale, 3 = RGB, 4 = RGBA) |
| data | [byte] | Raw interleaved pixel bytes, row-major order |

The length of `data` is always `width * height * channels`.

### imgfmt

A sum type representing the target container format for encoding.

| Variant | Meaning |
|---------|---------|
| Png | Lossless PNG |
| Jpeg | Lossy JPEG (quality controlled by the `quality` parameter) |
| Webp | WebP (lossy when quality < 100, lossless at quality = 100) |
| Bmp | Uncompressed BMP |

## Functions

### image.decode(bytes: [byte]): imgbuf!str

Decodes a PNG, JPEG, or WebP image from raw bytes. The format is detected automatically from the magic bytes; no extension or hint is required. Returns an error string on unrecognised data or a malformed file.

**Example:**
```toke
i=fs:std.file;
i=image:std.image;

let raw=fs.read_bytes("photo.jpg");
let result=image.decode(raw);
(match result{
  ok(buf){
    (* buf.width, buf.height, buf.channels, buf.data are populated *)
    (log.info("decoded " ++ str.from_u32(buf.width) ++ "x" ++ str.from_u32(buf.height)));
  };
  err(msg){
    (log.error("decode failed: " ++ msg));
  };
});
```

### image.encode(buf: imgbuf; fmt: imgfmt; quality: u8): [byte]!str

Encodes a pixel buffer to the given container format. `quality` applies to lossy formats (Jpeg, Webp) and is clamped to 1–100; it is ignored for Png and Bmp. Returns the encoded bytes, or an error string if the buffer dimensions are inconsistent with `data`.

**Example:**
```toke
(* Re-encode a decoded image as JPEG at 85 % quality *)
let result=image.encode(buf; $imgfmt.Jpeg; 85);
(match result{
  ok(jpeg_bytes){
    (fs.write_bytes("out.jpg"; jpeg_bytes));
  };
  err(msg){
    (log.error("encode failed: " ++ msg));
  };
});

(* Lossless PNG round-trip — quality value is ignored *)
let png_result=image.encode(buf; $imgfmt.Png; 0);
```

### image.resize(buf: imgbuf; width: u32; height: u32): imgbuf

Returns a new `imgbuf` scaled to `width` × `height` using bilinear interpolation. The channel count is preserved. This function is infallible; if either dimension is zero the returned buffer has zero pixels but the call succeeds.

**Example:**
```toke
(* Scale a decoded image to a 128 × 128 thumbnail *)
let thumb=image.resize(buf; 128; 128);
let encoded=image.encode(thumb; $imgfmt.Webp; 80);
```

### image.crop(buf: imgbuf; x: u32; y: u32; width: u32; height: u32): imgbuf!str

Returns a new `imgbuf` containing the rectangular region with its top-left corner at `(x, y)` and the given `width` and `height`. Returns an error string if the rectangle extends outside the source buffer.

**Example:**
```toke
(* Crop a 200 × 200 region from the centre of a 640 × 480 image *)
let result=image.crop(buf; 220; 140; 200; 200);
(match result{
  ok(cropped){
    (log.info("crop ok"));
  };
  err(msg){
    (log.error("crop out of bounds: " ++ msg));
  };
});
```

### image.to_grayscale(buf: imgbuf): imgbuf

Returns a new single-channel (grayscale) `imgbuf` by applying standard luminance weighting to the RGB channels. If the source already has one channel it is returned unchanged (copy). Alpha channels are dropped.

**Example:**
```toke
let gray=image.to_grayscale(buf);
(* gray.channels = 1 *)
(* gray.data.len = gray.width * gray.height *)
```

### image.flip_h(buf: imgbuf): imgbuf

Returns a new `imgbuf` that is a mirror image of `buf` along the vertical axis (left becomes right). Infallible.

**Example:**
```toke
let mirrored=image.flip_h(buf);
```

### image.flip_v(buf: imgbuf): imgbuf

Returns a new `imgbuf` that is a mirror image of `buf` along the horizontal axis (top becomes bottom). Infallible.

**Example:**
```toke
let flipped=image.flip_v(buf);
```

### image.pixel_at(buf: imgbuf; x: u32; y: u32): [byte]!str

Returns the pixel bytes at column `x`, row `y`. The slice length equals `buf.channels`. For an RGBA buffer this is `@(r; g; b; a)`. Returns an error string if `(x, y)` is outside the buffer dimensions.

**Example:**
```toke
let px_result=image.pixel_at(buf; 0; 0);
(match px_result{
  ok(px){
    (* px = @(r; g; b; a) for a 4-channel image *)
    (log.info("top-left red channel: " ++ str.from_u8(px.0)));
  };
  err(msg){
    (log.error(msg));
  };
});
```

### image.from_raw(data: [byte]; width: u32; height: u32; channels: u8): imgbuf

Wraps externally owned pixel bytes in an `imgbuf` without copying or validating. The caller is responsible for ensuring `data.len == width * height * channels`. Useful when integrating with camera or frame-capture APIs that already produce raw buffers.

**Example:**
```toke
(* Wrap a raw RGBA frame from a capture device *)
let w:u32=1920;
let h:u32=1080;
let ch:u8=4;
let frame_bytes=device.capture_raw();
let buf=image.from_raw(frame_bytes; w; h; ch);
let thumb=image.resize(buf; 320; 180);
```
