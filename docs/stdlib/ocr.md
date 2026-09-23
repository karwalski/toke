---
title: std.ocr
slug: ocr
section: reference/stdlib
order: 46
---

**Status: Implemented, macOS only** -- C runtime backing (story 135.6). **No library is vendored for this, and no new dependency is added.**

`std.ocr` recognises **text runs with positions** in a raster image, using the operating system's own text recogniser.

It is the other end of the route [`std.pdf`](/docs/stdlib/pdf) opens. `pdf.hastext` tells you which pages carry a text layer; the ones that do not are scans, and this is where they go.

## Read this before you use it

Two properties are unusual enough that using this module without knowing them will produce wrong results that look right.

### 1. It does not exist on every platform, and it says so

| Platform | Engine | `ocr.isavailable()` |
|---|---|---|
| macOS | Vision (`VNRecognizeTextRequest`) | `true` |
| Windows | -- | `false` |
| Linux, everything else | -- | `false` |

**Portability is poor by construction.** That is the accepted trade: accuracy is high, someone else maintains it, and it made the scanned path shippable immediately. [ADR-0015](/docs/decisions/ADR-0015/) refuses vendoring Tesseract, so the alternatives were this or waiting for a standalone component.

Where there is no engine, **every call fails with `$noengine`**. It never returns empty text. That distinction is the whole point: empty text reads as "this page has no content", which is a different and wrong answer, and a consumer that cannot tell them apart will silently file a scanned bank statement as a blank one.

So **route on `ocr.isavailable()`**, not on whether the result was empty.

### 2. The confidence is not a reliability signal

`$ocrrun` carries a `confidence`, because the platform reports one and throwing it away would be worse. **Do not build a threshold on it.**

Measured on macOS 26.5.1 across 40 images -- clean, faint, noisy, rotated, 5pt to 44pt -- at the default recognition level **every recognised run scored exactly `1.0`**, including these:

| The image reads | Vision returns | Confidence |
|---|---|---|
| `Illlll1I0O` | `\|\|\|\|\|1100` | **1.0** |
| `ACME` (6pt, 24x10px) | `VOME` | **1.0** |
| `ACME BANK` rotated 45 degrees | `= BANK` | **1.0** |

A consumer that flags a suspect field with `run.confidence < 0.9` **will never flag anything**. `ocr.topcandidates` would not help either: the engine returns exactly one candidate, so there is no second-place margin to compare against.

The full measurement is in [the 135.6 route note](/docs/decisions/135.6-platform-ocr-route/). Per-glyph and per-field confidence is [story 135.8](/docs/decisions/ADR-0015/)'s problem, and the finding above is an argument *for* that story rather than something this module can supply.

#### What you can use instead

**The engine refuses cleanly, and a refusal is information.** Zero runs from a page you can see is covered in text means the recogniser would not commit, and that is a far better flag than a confidence that is always full:

- text too small, too rotated or too degraded returns **no observation at all** rather than a guess;
- `ocr.setfast(true)` refuses much more readily than the default -- in the conformance suite it returns nothing for the confusable fixture that the default level reads confidently and wrongly. Recognising a page at **both** levels and comparing is a real signal that costs one extra pass.

## Coordinates

**Origin bottom-left, y increasing upward, unit = pixels of the source image.**

This follows [`std.pdf`](/docs/stdlib/pdf) rather than the raster convention, deliberately. The consumer this module exists for routes text pages to `pdf.runs` and scanned pages to `ocr.runs` and feeds both into one extractor, so the two must sort with the same comparison. Vision's own normalised box already uses the bottom-left origin; only the unit is converted.

If you want top-down screen coordinates, subtract from the image height -- the same conversion `pdf.pageheight` exists for.

## Types

### $ocrrun

**This is `std.pdf`'s `$textrun`, slot for slot.**

| Slot | `std.pdf` `$textrun` | `std.ocr` `$ocrrun` | Meaning here |
|---|---|---|---|
| 0 | `text` `$str` | `text` `$str` | The recognised text, UTF-8 |
| 1 | `page` `i64` | `page` `i64` | Always `1`; an image is one page |
| 2 | `x` `f64` | `x` `f64` | Pixels from the left |
| 3 | `y` `f64` | `y` `f64` | Pixels from the **bottom** |
| 4 | `width` `f64` | `width` `f64` | Pixels |
| 5 | `height` `f64` | `height` `f64` | Pixels |
| 6 | `fontsize` `f64` | **`confidence` `f64`** | As reported -- read the warning above |

Six of the seven fields are identical in name, type and position. The seventh differs in name only: same type, same slot. Under the struct ABI the two records are **bit-identical in layout**, so converting between them is a re-tag, not a re-pack.

The divergence is the honest one: **a PDF knows the font size and not the confidence; a recogniser knows the confidence and not the font size.** Keeping `fontsize` and writing `0.0` into it would have preserved the name at the cost of a field that is a lie in every row, and that somebody eventually divides by.

A run is **one observation** -- the engine's own unit, typically a line. It is not a word and not a paragraph.

### $ocrerr

The error side of every fallible call:

| Kind | When |
|---|---|
| `$noengine` | No recogniser on this platform. **Not** "no text" |
| `$badimage` | The bytes are not a decodable image |
| `$toosmall` | Below the engine's usable size; it would return a guess at full confidence |
| `$nolanguage` | A requested language the engine does not have |
| `$failed` | The engine itself returned an error |
| `$io` | The file could not be read |

Ask **`ocr.lasterrkind`** which one it was. The compiled error-union ABI lowers `T!E` to a null check, so the `$err` arm binds nothing a consumer can read. The kind is a closed, stable set and is meant to be switched on; `ocr.lasterr` is prose for a human and is not stable.

**A blank page is not an error.** It succeeds with zero runs and `ocr.lasterrkind` of `"none"`. Blank, unavailable and failed are three different answers.

## Functions

### ocr.isavailable(): bool

Whether a recogniser is actually usable here. **A real probe, not a compile-time constant**: it resolves the engine's classes, confirms every method this module calls is implemented, and asks the engine for its supported-language list -- which is the check that fails when the framework is present but its text model is not installed.

### ocr.engine(): $str

`"vision"` or `"none"`. This is the seam: route on this and on `ocr.isavailable()` and your code never names a platform, so a second engine can be added behind these same calls.

### ocr.languages(): $str

Comma-separated BCP-47 tags the engine supports, **queried live**. `""` when there is no engine. On macOS 26.5.1 this returns 30 tags beginning `en-US,fr-FR,it-IT,de-DE,...`.

### ocr.setlanguages(langs: $str): bool

Set the recognition languages, comma-separated, most-preferred first. `""` restores the default.

Returns `false` with kind `nolanguage` if **any** tag is one the engine does not have, and changes nothing. That check is not paranoia: Vision silently ignores a tag it does not know and recognises in its default language instead, which produces confident output in the wrong language. Note that a prefix is not a tag -- `"en"` is refused even though `"en-US"` is supported.

### ocr.setfast(fast: bool): void

`false` (the default) is the accurate level. `true` is roughly an order of magnitude faster and **refuses far more readily** -- see "What you can use instead" above.

### ocr.runs(data: @(byte)): @($ocrrun)!$ocrerr

Recognise an encoded image held in memory: PNG, JPEG, TIFF, BMP, GIF, HEIC -- whatever the host can decode.

### ocr.runsfile(path: $str): @($ocrrun)!$ocrerr

Read `path` and recognise it. Requires the `fs.read` capability (`--allow-read`).

**This is the route a large scan must take.** A toke `@(byte)` stores one byte per `i64` slot, so routing a 20 MB TIFF through `ocr.runs` costs 160 MB of toke array before a pixel is decoded.

### ocr.runsraw(px: @(byte); width: i64; height: i64; channels: i64): @($ocrrun)!$ocrerr

Recognise raw pixels. **This is the composition point with [`std.image`](/docs/stdlib/image)**: greyscale, crop, rotate and adaptive-threshold there, then recognise here with no re-encode in between.

Rows are tightly packed, **top row first** (std.image's layout); `channels` is 1, 3 or 4. A buffer too small for the stated geometry is refused rather than read off the end of.

### ocr.text(data: @(byte)): $str!$ocrerr
### ocr.textfile(path: $str): $str!$ocrerr

Every run joined with `\n`, **in the engine's order**.

Unlike `pdf.pagetext`, this does **not** sort. A PDF's emission order is arbitrary while its positions are exact, so sorting recovers real information; here the positions are themselves estimates and the engine already returns a sensible order for single-column text. A multi-column sort invented in this module would be a heuristic wearing the costume of a fact. Use `ocr.runs` and sort on the positions if you need that, with your own tolerance.

### ocr.lasterr(): $str
### ocr.lasterrkind(): $str

Prose for a human, and the stable kind to switch on. `lasterrkind` is one of `none`, `noengine`, `badimage`, `toosmall`, `nolanguage`, `failed`, `io`.

## The routing this exists for

Extraction is exact; recognition is probabilistic. Running OCR over a page that already has a text layer pays an error rate it never needed, so ask first.

```toke
m=statement;
i=io:std.io;
i=s:std.str;
i=p:std.pdf;
i=o:std.ocr;

(* A page WITH a text layer: exact, no error rate, no engine needed. *)
f=viapdf(doc:$pdfdoc;pageno:i64):i64{
  let t=p.pagetext(doc;pageno);
  mt t {
    $ok:txt io.println(s.concat("exact: ";txt));
    $err:e io.println(s.concat("pdf rejected: ";p.lasterrkind()))
  };
  <0
};

(* A scanned page. Rasterise it to PNG however you like, then: *)
f=viaocr(path:str):i64{
  let t=o.textfile(path);
  mt t {
    $ok:txt io.println(s.concat("recognised: ";txt));
    $err:e io.println(s.concat("ocr rejected: ";o.lasterrkind()))
  };
  <0
};

(* Route on hastext -- and on isavailable, NOT on whether text came back
   empty, because "no engine here" and "this page is blank" are different
   facts and only one of them means you should stop. *)
f=routepage(doc:$pdfdoc;pageno:i64;scan:str):i64{
  if(p.hastext(doc;pageno)){
    viapdf(doc;pageno)
  }el{
    if(o.isavailable()){
      viaocr(scan)
    }el{
      io.println(s.concat("scanned page, and no recogniser on this host: ";o.engine()))
    }
  };
  <0
};

f=main():i64{
  let d=p.openfile("statement.pdf");
  mt d {
    $ok:doc routepage(doc;1;"page1.png");
    $err:e io.println(s.concat("cannot open: ";p.lasterrkind()))
  };
  <0
};
```

## Limits and configuration, in one place

| Constant (`src/stdlib/ocr.h`, `ocr.c`) | Value | What it bounds |
|---|---|---|
| `OCR_MIN_DIMENSION` | 20 px | Either dimension below this is refused `$toosmall` rather than guessed at |
| `OCR_MAX_LANGS` | 16 | Language tags accepted by `ocr.setlanguages` |

The recognition **revision is pinned** to the newest the host's engine supports, rather than left to the default. Revision is not cosmetic: the same image scores confidence `1.0` at revision 3 and `0.5` at revision 2. An unpinned request silently follows whatever the next OS update ships, so a suite that passes today fails later for a reason nothing in the diff explains.

Language correction is **off**. It rewrites recognised text toward dictionary words, which on a statement turns account numbers and reference codes into English -- invisibly, and in exactly the fields that matter.

## See Also

- [std.pdf](/docs/stdlib/pdf) -- the other end of this route; `pdf.hastext` is what decides between them
- [std.image](/docs/stdlib/image) -- decode, greyscale, crop, rotate and threshold before recognising
- [The 135.6 route note](/docs/decisions/135.6-platform-ocr-route/) -- the probe measurements, and why the confidence is what it is
- [ADR-0015](/docs/decisions/ADR-0015/) -- why no OCR library is vendored
