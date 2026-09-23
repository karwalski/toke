# Reply to loke — the document and image libraries round

**From:** toke
**To:** the loke project
**Date:** 2026-09-23
**Re:** `loke/docs/toke-libraries-required.md` v1.0 (2026-09-19)
**Verified against:** toke `3.0.0`, commit `453dd14`
**Status:** four of six shipped, one shipped in a different form, one answered with a decision rather than code

---

## The position, stated first

**Five of your six items are in the tree and under a conformance suite each.** The sixth, OCR, is
answered in three parts — Tesseract refused, a platform binding **shipped**, and a standalone
`toke-ocr` accepted as the long project — and the reasoning behind all three is not the obvious one.
**One item within PDF (embedded images) is declined for now**, and was declined in advance rather
than discovered missing at the end.

Nothing here changes your MK21 position: it ships CSV only, states that limitation plainly, and
handles extraction through a sidecar. That remains correct, and **this is a retire-the-sidecar
programme, not a critical path**. Adopt these when it suits you.

**The request was treated as the specification, and it earned that.** It stated what toke already had
before asking for anything, it named the constraints that rule options out, and in Section 2 it argued
against the obvious answer in the one place the obvious answer is wrong. Two of its asides turned out
to be load-bearing and both were right:

- *"toke already vendors zlib-equivalent inflate for other purposes; confirm before assuming."* We
  confirmed. `zlib` is on the link line of every toke binary already, so `FlateDecode` in `std.pdf`
  cost **no new dependency at all**.
- *"treat 'extracted something' as a much weaker result than 'extracted correctly'."* This caught a
  real defect. See [2](#2-pdf-text-extraction--accepted-with-one-interface-change-and-one-deferral).

Where we changed an interface you specified, the change and the reason are below. Where we did not
ship something, it says so plainly.

---

## The constraint that decides Section 5, named explicitly as you asked

**ADR-0015 — "The toke toolchain is C99 only; no vendored C++ dependencies"** was written and ratified
on 2026-09-22, specifically because your document forced the question. `CFLAGS` is `-std=c99`, there
is no `CXX` and no C++ link step, and there are zero C++ files in the tree — verified against the git
index rather than the working copy.

Vendoring a C++ dependency is not an addition; it imposes a C++ toolchain on every toke build on every
platform, permanently. **That is why Tesseract is refused outright** despite being the obvious OCR
answer, and it is why the answer in Section 5 is not the one you would expect.

The ADR records two escape hatches as first-class routes rather than caveats — a C shim around an
out-of-tree C++ library, and a separate optional non-default component — so a future request knows what
*would* be considered. It also explicitly does not foreclose moving to a later C standard: the
constraint is C++, not C99 forever.

---

## Item by item

### 1. CSV hardening — **accepted as specified**

Shipped. `std.csv` now carries 18 exports including the two you named — `csv.sniff` and
`csv.readeropts` — with `$csvopts` carrying delimiter, quote character, encoding, BOM handling and a
ragged-row policy, and `csv.raggedreport` for the report policy.

Every item in your table is handled: BOM detection and stripping, declared-or-detected encoding with a
loud failure rather than mojibake, a delimiter sniffer over the first N lines that **reports** what it
found, RFC 4180 quoting across line breaks, a caller-chosen ragged-row policy, and **no numeric
coercion** — the raw-string accessor stays the default, so leading zeros in a BSB survive.

### 2. PDF text extraction — **accepted, with one interface change and one deferral**

Shipped as `std.pdf`, 11 exports, `docs/stdlib/pdf.md`. Items 1 through 4 of your priority list are
complete.

**Interface deltas, all of them additive except one rename:**

| You specified | We shipped | Why |
|---|---|---|
| `pdf.textruns(doc; page)` | `pdf.runs(doc; page)` | Shorter and unambiguous in context; the only rename |
| `$textrun{text; x; y; width; height; fontsize}` | plus `page` | A run carries its own page, so a caller merging pages does not have to track it |
| — | `pdf.pagetext(doc; page)` | `pdf.runs` returns **content-stream order**; `pdf.pagetext` is the one that sorts into reading order. Reading order is derivable from positions; emission order is derivable from nothing, so the unsorted form had to be the primitive |
| — | `pdf.openfile(path)` | `pdf.open` takes `@(byte)`, which costs eight bytes per byte. A file on disk should not pay that |
| — | `pdf.pagewidth` / `pdf.pageheight` | A bounding box is not interpretable without the page box |
| `$pdferr` incl. Encrypted, Malformed | six variants | `$baddoc`, `$encrypted`, `$nopage`, `$toolarge`, `$unsupported`, `$io` |
| `pdf.images(doc; page)` | **not shipped** | Declined for now — see below |

**Two design decisions you should know about, because they affect how you write the caller:**

- **A run is one text-showing operator** — the producer's own unit of text. The module never guesses
  where a column boundary is, which means your table reconstruction sees exactly what the producer
  emitted and nothing invented on top.
- **An unmappable character code becomes U+FFFD**, never a guess. Visibly wrong beats plausibly wrong,
  which is your own Section 2 warning applied as a rule.

**Your warning was right, and here is the proof.** A fixture using a Type0 CID font under
`/UniJIS-UCS2-H` found that `/W` is indexed by **CID, not by the content-stream code**. The extractor
was returning a real width belonging to a *different* glyph — **94.094pt where the true width is
102.284pt** — and nothing about the number looked wrong. It is fixed, and it was found by a fixture
rather than by reading the code, which is the argument for your "budget for a fixture suite" line.

**Route: we wrote a purpose-built C99 extractor rather than vendoring.** Your candidate list was
accurate and we costed all of it. pdfium and podofo fall to ADR-0015. **poppler is disqualified twice**
— C++ *and* GPL-2.0+ against this repository's Apache-2.0. **MuPDF fails on licence before it fails on
anything technical**: it is AGPL-3.0-or-later, and the toke standard library is statically linked into
*your* program, so vendoring it would attach network copyleft to every binary `tkc` compiles. The
`dlopen` hatch does not escape that — dynamic combination is still combination. Searching for a
permissive C equivalent came up empty; every mature extractor is C++, JavaScript, Python or Java. So
writing one was the only remaining option. Your estimate that the required subset is "far smaller than
a full renderer" held: ~3,000 lines.

**Embedded image extraction (your item 5) is not shipped.** It was scoped out in the route note before
implementation rather than dropped at the end. It is the item that feeds the OCR path, and the OCR path
is itself not ready — so shipping it now would deliver a producer with no consumer. Tell us if that
ordering is wrong for you.

**Honestly untested, so you do not discover it the hard way:** the Identity-H composite path (our CID
fixture uses a predefined CMap), predefined CJK CMaps, form XObjects, inline images, rotated or skewed
text, and encrypted-with-empty-user-password documents. There is also **no performance measurement on
multi-megabyte documents, and correspondingly no performance claim in the documentation.**

### 3. Zip archive reading — **accepted as specified**

Shipped as `std.zip`, 6 exports, on **miniz** as you suggested. Your three calls are there verbatim in
shape (`zip.open`, `zip.entries`, `zip.read`), plus `zip.openfile`, `zip.close` and `zip.lasterr`.
Read-only, as you said was sufficient.

**Every security constraint you listed is enforced, and they are not optional here either:** path
traversal rejected on any `..` component, archive size capped at 128 MiB, cumulative uncompressed size
capped at 64 MiB, compression ratio capped at 200:1, entry count capped at 65536, entry name length
capped at 511 bytes. One you did not ask for: **a backslash in an entry name is rejected outright**
rather than treated as a separator, because `..\..\x` is a traversal that a forward-slash-only check
misses entirely, and no XLSX, ODS, DOCX or EPUB legitimately contains one.

The caps are compile-time constants deliberately: a bound settable from toke is under the control of
whichever code path is handling the hostile input.

### 4. XLSX reading — **accepted as specified**

Shipped as `std.xlsx`, 8 exports, on `std.zip` — **no second library vendored**, as you required.
Your three calls are there; `$workbook` is spelled `$xlsxbook` and `$cell` is `$xlsxcell`, and the cell
carries `ref`, `row`, `col`, `celltype`, the raw string and the resolved value.

**All three of the traps you named are covered, and the conformance suite asserts them premise-first**
— it asserts what the independent encoder actually wrote *before* asserting what toke reads, so each
trap is proved present in the fixture rather than assumed:

1. **Shared strings** — pool index is not position, and inline strings and XML entities are both covered.
2. **The 1900 leap-year bug is reproduced, not fixed.** Serial 60 is the phantom 1900-02-29, 59 is
   1900-02-28, 61 is 1900-03-01. The 1904 Mac system declares itself, has no phantom, and serial 0 is
   its epoch.
3. **Sparse rows** — `xlsx.rows` returns the covering rectangle with absent cells *present* and typed
   `empty`, so **position never has to be inferred from order**. Interior gaps and wholly missing rows
   are both asserted not to shift their neighbours.

Scope held exactly as you asked: cached formula values only, no evaluation, no styling, no charts, no
pivot tables.

**One measured number you will want.** The binding limit is the cell cap, not the archive caps: a
1,040,000-cell workbook is refused by `TK_XLSX_MAX_CELLS` while its 43.3 MB of worksheet XML is still
comfortably inside `std.zip`'s 64 MiB ceiling. At ~41 bytes of XML per populated cell that ceiling
would not bind until roughly 1.6 million cells. **So if a real input needs more, the cell cap is the
one to raise.** For scale: 480,000 cells is a 2.2 MB file, 0.22 s to parse, 138 MiB peak RSS.

### 5. OCR — **Tesseract refused; platform binding accepted and not yet built; `toke-ocr` accepted as a separate project**

This is the one item where you get a decision rather than code, and the three routes you compared get
three different answers.

- **Route: vendor Tesseract — refused.** See ADR-0015 above. Not a capacity judgement; a permanent
  change to what toke is.
- **Route: platform OCR binding — accepted, and now shipped as `std.ocr`.** macOS Vision, bound from
  plain C99 via `objc_msgSend` with no Objective-C compiler and no `.m` file, following the
  `src/stdlib/webview.c` precedent — so it cost no toolchain change and ADR-0015 is untouched. Twelve
  exports, none of which spells "Vision"; off macOS every entry point returns `$noengine` rather than
  empty text. `$ocrrun` is `$textrun` slot for slot — six of seven fields identical in name, type and
  position, the seventh differing in name only — so **an OCR fallback on a page `pdf.hastext` says has
  no text needs one record type, not two**.

  **One finding you need before you design around this, and it is not the answer we expected.**
  **Vision's confidence carries no signal.** Across 40+ images at the default accuracy level,
  `confidence` was **1.0 for every recognised candidate, including flatly wrong output** — an image
  reading `Illlll1I0O` came back as `I111111I00` at confidence 1.0; `ACME` at 6pt came back as `VOME`
  at 1.0; `ACME BANK` rotated 45° came back as `= BANK` at 1.0. `topCandidates:` returns exactly one
  candidate, so the top-two margin is not available either. **A `confidence < 0.9` check flags
  nothing.** We surface the value unaveraged because it is what the platform reports, but treat it as a
  per-level label, not a measurement.

  **What to route on instead: refusal.** Vision returns zero observations rather than guessing, and the
  fast level refuses a confusable fixture that the accurate level reads confidently and wrongly. An
  empty result is informative here; a high confidence is not.

  This also means **per-field confidence is an argument for `toke-ocr` rather than something this
  binding lets you defer** — the opposite of how we had it sequenced. If confidence-gated extraction is
  load-bearing for your statement reader, say so, because it changes which of the two we push on.
- **Route: `toke-ocr` as a standalone repository — accepted as you argued it.** Your case for keeping
  it out of the standard library is the one we adopted: PDF and zip are bounded, deterministic,
  fixture-testable parsers; OCR is a recognition system with an accuracy distribution, model artefacts
  and a training pipeline. Putting it in `stdlib/` would mean the standard library ships model weights
  and acquires an accuracy metric.

**Two things we will hold ourselves to when that repository exists**, both from your own document:
per-glyph and per-field confidence is load-bearing and must be designed in rather than bolted on, and
**no aggregate accuracy figure gets published without the distribution behind it**. Its README will say
plainly that it will start out less accurate than Tesseract and may stay so for a long time, so nobody
adopts it under a misapprehension.

### 6. Image preprocessing gaps — **accepted as specified, all four**

Shipped in `std.image`: `image.rotate` (arbitrary angle with interpolation), `image.adaptivethreshold`
(Sauvola), `image.blur` and `image.convolve` (general kernel), and `image.tiffdecode` with
`image.tiffpages` for multi-page TIFF.

**An honest note on two of them.** `image.rotate` and `image.blur` were **already implemented in the C
core and simply unreachable from toke** — the work was exposing them, not writing them. That is a
defect class we have since gated against, and it is worth your knowing because it means these two have
been available in the C for longer than the interface suggested.

Still missing, and now tracked: **TIFF CCITT G3/G4 decode**, which matters specifically for
fax-derived documents.

---

## Not requested, shipped anyway, because Section 2 and 4 could not have worked without them

- **Binary file read.** toke had no way to read a file as bytes at all. Both `std.pdf` and `std.xlsx`
  need one.
- **A bounded read above the 64 MiB byte-array cap** — `file.readrange(path; offset; len)` plus
  `file.size(path)`. **Bounded rather than streaming, for a reason specific to your two formats: both
  are indexed from their own end** — a PDF through `startxref` in its trailer, a zip (so every XLSX)
  through the end-of-central-directory record. Neither can be parsed forward from the start, so a
  stream would have been the wrong primitive for both.
- **toke's version was moved to 3.0.0 in the tree, but the release is not out yet — so keep pinning the
  commit hash for now.** `VERSION` had read 2.8.0 since 2026-06-21, across the whole v0.4 break, which
  is why ooke had to invent `mintoke` + `mintokedate` + `mintokecommit` to express a minimum it could
  not state as a version, and why you are pinning a hash. `VERSION`, `src/main.c` and the release
  workflow now all say 3.0.0 and a gate keeps them agreeing. **But `v3.0.0` is not tagged — the newest
  tag is `v2.8.0` — and the homebrew formula points at a release URL that does not exist yet.** Tagging
  and publishing is an owner action still outstanding. We are telling you this rather than letting you
  discover it, because "the version was cut" would read as "there is a release" and there is not.
  **We will tell you when `v3.0.0` is published; pin `>= 3.0.0` then, not before.**

---

## What we are asking for

Nothing urgent. Three things, in order of usefulness to us:

1. **Tell us if declining embedded image extraction ordered it wrong.** Our reasoning is that it feeds
   the OCR path and the OCR path is not ready. If you have a use for it before then, say so.
2. **A real-document fixture, if you can share one that is safe to share.** Our PDF fixtures are all
   producer-generated. A redacted real bank statement — or just the name of the producer that generates
   yours — would tell us more about the font-encoding paths than any synthetic case can.
3. **Whether confidence-gated extraction is load-bearing for you.** macOS Vision is shipped, but its
   confidence cannot support a threshold (above). If your statement reader needs to flag a low-confidence
   field rather than accept it, that pushes `toke-ocr` up rather than leaving it as the long project —
   and if Windows or Linux matters more than macOS, that changes the order again.

## What we are not asking for

Any change on your side. These libraries are additive, the interfaces above are what will be there, and
**nothing in this document has to be adopted on any schedule** — the sidecar keeps working.

---

## Appendix: where each thing lives

| Item | Module | Exports | Documentation | Conformance |
|---|---|---|---|---|
| 1. CSV hardening | `std.csv` | 18 | `docs/stdlib/csv.md` | existing suite |
| 2. PDF text extraction | `std.pdf` | 11 | `docs/stdlib/pdf.md` | `C032`, 92 assertions |
| 3. Zip archive reading | `std.zip` | 6 | `docs/stdlib/zip.md` | existing suite |
| 4. XLSX reading | `std.xlsx` | 8 | `docs/stdlib/xlsx.md` | `C031`, 52 assertions |
| 5. OCR | `std.ocr` | 12 | `docs/stdlib/ocr.md` | `C033`, 76 assertions |
| 6. Image preprocessing | `std.image` | 15 | `docs/stdlib/image.md` | `C030` |
| Route reasoning for 2 | — | — | `docs/decisions/135.3-pdf-extraction-route.md` | — |
