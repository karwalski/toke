---
title: std.image
slug: image
section: reference/stdlib
order: 19
---

**Status: Implemented** -- C runtime backing.

The `std.image` module provides functions for decoding, encoding, and transforming raster
images. It decodes PNG and TIFF and encodes PNG and BMP; JPEG and WebP encode and decode
return an error naming the library that would be needed.

Beyond the basic transforms it carries the pieces a document pipeline needs before OCR or
analysis: arbitrary-angle rotation for deskew, adaptive thresholding for unevenly lit
scans, a general convolution for noise reduction, and multi-page TIFF decode.

## Types

### $imgbuf

| Field | Type | Meaning |
|-------|------|---------|
| width | u32 | Image width in pixels |
| height | u32 | Image height in pixels |
| channels | u8 | Number of color channels (3=RGB, 4=RGBA) |
| data | @($byte) | Raw pixel data |

### $imgfmt (sum type)

| Variant | Meaning |
|---------|---------|
| $Png | PNG format |
| $Jpeg | JPEG format |
| $Webp | WebP format |
| $Bmp | BMP format |

## Functions

| Function | Parameters | Return | Description |
|----------|-----------|--------|-------------|
| `image.decode` | `data: @($byte)` | `$imgbuf!$str` | Decode an image from bytes (auto-detects format) |
| `image.encode` | `img: $imgbuf; fmt: $imgfmt; quality: u8` | `@($byte)!$str` | Encode an image to bytes in the given format |
| `image.resize` | `img: $imgbuf; w: u32; h: u32` | `$imgbuf` | Resize an image to the given dimensions |
| `image.crop` | `img: $imgbuf; x: u32; y: u32; w: u32; h: u32` | `$imgbuf!$str` | Crop a rectangular region |
| `image.tograyscale` | `img: $imgbuf` | `$imgbuf` | Convert to grayscale |
| `image.fliph` | `img: $imgbuf` | `$imgbuf` | Flip horizontally |
| `image.flipv` | `img: $imgbuf` | `$imgbuf` | Flip vertically |
| `image.pixelat` | `img: $imgbuf; x: u32; y: u32` | `@($byte)!$str` | Get pixel channel values at (x, y) |
| `image.fromraw` | `data: @($byte); w: u32; h: u32; channels: u8` | `$imgbuf` | Construct an image buffer from raw pixel data |

| `image.rotate` | `img: $imgbuf; angledeg: f64` | `$imgbuf` | Rotate by an arbitrary angle about the centre, bilinear |
| `image.blur` | `img: $imgbuf; passes: u32` | `$imgbuf` | Apply the 1/2/1 Gaussian kernel `passes` times |
| `image.convolve` | `img: $imgbuf; kernel: @($f64); ksize: u32; divisor: f64; offset: f64` | `$imgbuf!$str` | Apply an arbitrary odd-sized kernel |
| `image.adaptivethreshold` | `img: $imgbuf; window: u32; k: f64` | `$imgbuf!$str` | Sauvola local thresholding to a 0/255 plane |
| `image.tiffpages` | `data: @($byte)` | `u32!$str` | Number of pages (IFDs) in a TIFF |
| `image.tiffdecode` | `data: @($byte); page: u32` | `$imgbuf!$str` | Decode one 0-based page of a TIFF |

## Document preprocessing

### image.rotate

`fliph` and `flipv` cover the two right angles. Deskewing a scan does not: the correction is
typically a fraction of a degree, so the function takes an arbitrary angle in degrees and
resamples with **bilinear interpolation** — the four nearest source pixels, weighted by
distance. Bilinear is the baseline choice here because it is separable, cheap, and introduces
no overshoot; nearest-neighbour would leave visible stair-stepping on text edges, and bicubic
costs four times as much while sharpening the ringing that thresholding then turns into
speckle.

The output is the same size as the input and rotates about the centre, so corners that rotate
in from outside the frame are filled with 0.

### image.adaptivethreshold

**Global thresholding fails on unevenly lit scans, which is most scans.** One cut value for the
whole page loses the text wherever the page is in shadow, or the background wherever it is
bright.

This function uses **Sauvola** local thresholding. For each pixel it takes the mean `m` and
standard deviation `s` of the `window` x `window` neighbourhood and thresholds at

```
T = m * (1 + k * (s / 128 - 1))
```

The mean term is what follows the illumination. The standard-deviation term is what separates
Sauvola from a plain local mean: over blank paper `s` falls towards zero, `T` drops well below
the background, and the region stays white instead of breaking into speckle — which is exactly
what a local-mean threshold does to a margin.

`window` is forced odd and to at least 3; passing 0 selects the default of 31. `k` is
conventionally 0.2; larger values keep more of the page white, smaller values more black. Input
of any channel count is reduced to luminance first, and the result is a single-channel image
whose pixels are exactly 0 or 255. Cost does not depend on the window size, because the means
come from integral images.

### image.convolve

A general kernel rather than another fixed filter. `kernel` holds `ksize * ksize` values in
row-major order and `ksize` must be odd. Each output channel is

```
out = sum(kernel[i] * src[i]) / divisor + offset
```

clamped to 0..255, with clamp-to-edge sampling at the borders. A `divisor` of `0.0` means
"normalise by the sum of the kernel", which is what a blur wants; a kernel that sums to zero
(Sobel, Laplacian) is then left unscaled rather than divided by zero. `offset` is applied after
the division, so an edge kernel can be recentred on 128.

### TIFF

Scanners and fax gateways produce TIFF, and a scanned document is usually **multi-page** — one
IFD per page. `image.tiffpages` reports how many there are and `image.tiffdecode` takes a
0-based page index. `image.decode` also auto-detects TIFF and returns page 0, so existing
callers gain single-page support without changing.

Supported: both byte orders; compression 1 (none), 5 (LZW), 8 and 32946 (Deflate) and 32773
(PackBits); 1, 4, 8 and 16 bits per sample, with 16-bit reduced to its high byte;
WhiteIsZero, BlackIsZero, RGB and Palette photometrics; and Predictor 2 (horizontal
differencing) on 8-bit samples.

Not supported, each rejected with a message naming what was found rather than a generic
failure: **CCITT G3/G4 fax compression**, JPEG-in-TIFF, tiled layouts, PlanarConfiguration 2
and BigTIFF.

## Usage

```toke
m=example;
i=image:std.image;
i=file:std.file;

f=main():i64{
  let raw = mt file.read("photo.jpg") {$ok:d d;$err:e ""};
  let img = mt image.decode(raw) {$ok:i i;$err:e $imgbuf{width:0;height:0;channels:0;data:@()}};
  let thumb = image.resize(img; 128; 128);
  let out = mt image.encode(thumb; $imgfmt{$Png:true}; 90) {$ok:b b;$err:e @()};
  let res = file.write("thumb.png"; out);
  <0;
}
```

## Dependencies

None.

## Example: preparing a scanned page

Deskew, denoise, then binarise — the order a document pipeline uses, because rotation and
blurring both change local statistics that the threshold then reads.

```toke
m=prep;
i=image:std.image;
i=file:std.file;
i=str:std.str;
i=io:std.io;

f=main():i64{
  let empty = $imgbuf{width:0;height:0;channels:0;data:@()};

  let raw = mt file.readbytes("scan.tif") {$ok:d d;$err:e @()};
  let pages = mt image.tiffpages(raw) {$ok:n n;$err:e 0};
  io.println(str.concat("pages: "; str.fromint(pages)));

  let page = mt image.tiffdecode(raw; 0) {$ok:i i;$err:e empty};

  (* deskew by the measured angle, then a light 3x3 blur to kill scanner noise *)
  let straight = image.rotate(page; 1.75);
  let smooth = image.blur(straight; 1);

  (* Sauvola: survives a page lit unevenly, which a global cut does not *)
  let bw = mt image.adaptivethreshold(smooth; 31; 0.2) {$ok:i i;$err:e empty};

  let out = mt image.encode(bw; $imgfmt{$Png:true}; 90) {$ok:b b;$err:e @()};
  let wrote = mt file.writebytes("page0.png"; out) {$ok:v v;$err:e false};
  <0
}
```

## Example: a custom kernel

`image.convolve` takes any odd-sized kernel, so a sharpen, an emboss or an edge detector is a
literal rather than a new function. This one is a 3x3 Laplacian edge detector: it sums to zero,
so `divisor` is left at `0.0` and the result is recentred on 128.

```toke
m=edges;
i=image:std.image;
i=file:std.file;

f=main():i64{
  let empty = $imgbuf{width:0;height:0;channels:0;data:@()};

  let raw = mt file.readbytes("photo.png") {$ok:d d;$err:e @()};
  let img = mt image.decode(raw) {$ok:i i;$err:e empty};
  let grey = image.tograyscale(img);

  let lap = @(0.0;1.0;0.0; 1.0;0.0-4.0;1.0; 0.0;1.0;0.0);
  let out = mt image.convolve(grey; lap; 3; 0.0; 128.0) {$ok:i i;$err:e empty};

  let png = mt image.encode(out; $imgfmt{$Png:true}; 90) {$ok:b b;$err:e @()};
  let wrote = mt file.writebytes("edges.png"; png) {$ok:v v;$err:e false};
  <0
}
```
