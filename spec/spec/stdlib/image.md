# std.image

Image decoding, encoding, and pixel-level manipulation for PNG, JPEG, WebP, and BMP formats.

## Types

```
type imgbuf { width: u32, height: u32, channels: u8, data: [byte] }
sum  imgfmt = Png | Jpeg | Webp | Bmp
```

## Functions

```
f=decode(data:[byte]):imgbuf!str
f=encode(img:imgbuf;fmt:imgfmt;quality:u8):[byte]!str
f=resize(img:imgbuf;width:u32;height:u32):imgbuf
f=crop(img:imgbuf;x:u32;y:u32;w:u32;h:u32):imgbuf!str
f=to_grayscale(img:imgbuf):imgbuf
f=flip_h(img:imgbuf):imgbuf
f=flip_v(img:imgbuf):imgbuf
f=pixel_at(img:imgbuf;x:u32;y:u32):[byte]!str
f=from_raw(data:[byte];width:u32;height:u32;channels:u8):imgbuf
```

## Semantics

- `decode` detects the format from file headers and decodes into an `imgbuf`. Returns error string on unsupported or corrupt data.
- `encode` compresses to the given format. `quality` is 0-100 (used by JPEG and WebP; ignored by PNG and BMP). Returns error on invalid buffer dimensions.
- `resize` uses bilinear interpolation. Aspect ratio is not preserved; caller must compute proportional dimensions.
- `crop` extracts a sub-rectangle. Returns error if the region extends beyond image bounds.
- `to_grayscale` converts to single-channel (channels becomes 1). Already-grayscale images are returned unchanged.
- `flip_h` / `flip_v` mirror horizontally / vertically.
- `pixel_at` returns the raw channel bytes at (x, y). Returns error if coordinates are out of bounds.
- `from_raw` wraps raw pixel data into an `imgbuf`. `data` length must equal `width * height * channels`.

## Dependencies

None.
