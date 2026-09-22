---
title: std.pdf
slug: pdf
section: reference/stdlib
order: 45
---

**Status: Implemented** -- C runtime backing (story 135.3). **No library is vendored for this, and no new dependency is added.**

`std.pdf` extracts **text runs with positions** from a PDF.

Positions are the point. A bank statement is a table, an invoice is a table, and a flat string has thrown the columns away before the caller ever sees it. Every run carries the page it came from and where on that page it sits, so a consumer can group by column, sort into reading order, or align two documents against each other.

## Text PDFs and scanned PDFs are different problems

Most statements, invoices and government reports are **text PDFs**: the characters are in the file and come out exactly. Only a scan needs OCR.

Conflating the two means paying OCR's error rate on documents that never required it. So this module does extraction only, and tells you which kind of document you have with [`pdf.hastext`](#pdfhastextdoc-pdfdoc-page-i64-bool). OCR is [story 135.6 / 135.8](/docs/decisions/ADR-0015/), a separate capability behind a platform binding or a separate component.

## Why an extractor was written rather than a library vendored

[ADR-0015](/docs/decisions/ADR-0015/) leaves the toolchain C99 only, which disqualifies pdfium, podofo and poppler. The only mostly-C engine, MuPDF, is **AGPL-3.0-or-later** against this repository's Apache-2.0 -- and because the stdlib is statically linked into *your* program, vendoring it would attach a network-copyleft obligation to every binary anyone compiles with `tkc`. There is no permissively-licensed C99 PDF text extractor to find.

The full costing is in [the route note](/docs/decisions/135.3-pdf-extraction-route/). The subset actually needed -- parse the objects, inflate the Flate streams, walk the content stream for the text-showing operators, apply the font's encoding map -- is two orders of magnitude smaller than an engine that is almost all rasteriser.

**zlib adds nothing.** It is already on every toke binary's link line.

## The four things that catch every first implementation

Each produces **output that looks like text** rather than a failure. That is why `test/conform/C032_pdf_text_runs.sh` asserts exact strings at exact positions, and never on "something came out".

### 1. A byte in a content stream is a glyph code, not a character

What it means is decided by the font's `/ToUnicode` CMap, or by `/Encoding /Differences` over a base encoding, or by the font's built-in encoding -- in that order. Read the bytes directly and a WinAnsi document comes out right and every other one comes out subtly wrong.

### 2. One code can be several characters

`fi` is one glyph. It is U+FB01 in the standard encodings, and an embedded font's `/ToUnicode` may map its code to the *two* characters `fi`. A one-code-one-character reader drops the second half and produces `f` where the page shows `fi` -- which is still a word.

### 3. Sometimes there is no mapping at all, and the honest answer is to say so

An embedded subset font with no `/ToUnicode`, or a composite font using a predefined CMap this module does not implement, contains **no information** about what its codes mean. A full renderer does no better except by heuristic.

`std.pdf` does not guess. An unmappable code is emitted as **U+FFFD**, so the failure is visible in the output instead of plausible. `pdf.hastext` still reports `true` for such a page -- it *has* a text layer, and OCR is not the remedy for a missing `/ToUnicode`.

The same rule governs widths. `/W` is indexed by **CID**, and the content-stream code is the CID only under `Identity-H`. Under a predefined CMap, indexing `/W` by the code returns a real width belonging to a *different* glyph; measured on one fixture, 94.094pt where the truth was 102.284pt, with nothing about the number looking wrong. So in that case every glyph gets `/DW`: uniformly approximate rather than selectively wrong.

### 4. Emission order is not reading order

Producers emit text objects in whatever order suits them, which is frequently bottom-up, right-to-left, or column by column.

`pdf.runs` returns runs in **content-stream order**, deliberately. That is the file's own order; nothing is gained by hiding it, and reading order is recoverable from the positions while emission order is recoverable from nothing. [`pdf.pagetext`](#pdfpagetextdoc-pdfdoc-page-i64-strpdferr) is the one that sorts.

## What a run is

**One text-showing operator** -- one `Tj`, `TJ`, `'` or `"`.

That is the producer's own unit of text: a table cell is one `drawString` is one `Tj`. Using it means `std.pdf` never has to *guess* where a column boundary lies, which is the most common way a PDF extractor invents structure the file does not contain. Merging adjacent runs would destroy the columns; splitting finer would fabricate boundaries.

## Coordinates

PDF default user space for the page: **points** (1/72"), origin at the **bottom-left**, **y increasing upward**. They are not flipped to screen convention -- the convention you want is your business, and flipping silently is how everything ends up upside down. `pdf.pageheight` is there for the conversion.

| Field | Meaning |
|---|---|
| `x`, `y` | The pen position at the first glyph. **`y` is the baseline**, not the bottom of the box -- it is the number the producer passed to its own `drawString` |
| `width` | The run's total advance, measured **along the baseline direction** |
| `height` | The em box: (ascent - descent) x size, from the font descriptor where the file has one and 1.0 em where it does not |
| `fontsize` | The **effective** size after the text and current transformation matrices, not the raw `Tf` operand |

So the box is `[x, x + width]` x `[y + descent, y + descent + height]`, with `descent` negative.

`fontsize` being *effective* matters: a document that sets a 1pt font and scales it by 12 in `Tm` is showing 12pt text, and reporting 1 would be a plausible wrong answer.

Rotated or skewed text keeps exact `x`, `y` and `fontsize`, and `width` is measured along the baseline rather than along the page's x axis. There is no axis-aligned box for rotated text and pretending there is one would be a fifth plausible wrong answer.

## Types

### $pdfdoc

An opaque handle. Release it with `pdf.close`; everything derived from it is dangling afterwards.

### $textrun

| Field | Type | Meaning |
|---|---|---|
| `text` | `$str` | The run's text, UTF-8 |
| `page` | `i64` | 1-based page number |
| `x` | `f64` | Points from the left of the page |
| `y` | `f64` | Points from the bottom; the **baseline** |
| `width` | `f64` | Advance width in points |
| `height` | `f64` | Em box height in points |
| `fontsize` | `f64` | Effective size in points |

### $pdferr

The error side of every fallible call: `$baddoc`, `$encrypted`, `$nopage`, `$toolarge`, `$unsupported`, `$io`.

Ask **`pdf.lasterrkind`** which one it was. The compiled error-union ABI lowers `T!E` to a null check, so the `$err` arm binds nothing a consumer can read -- and a password-protected statement that came back as "no text" would read as a statement with nothing in it. The kind is a closed, stable set and is meant to be switched on; `pdf.lasterr` is prose for a human and is not stable.

## Functions

### pdf.open(data: @(byte)): $pdfdoc!$pdferr

Opens a document held in memory. Builds the object index, resolves the page tree, and **refuses an encrypted document**. No page content is decoded here: a 400-page report costs one page's work when one page is wanted.

### pdf.openfile(path: $str): $pdfdoc!$pdferr

Reads `path` and opens it. Requires the `fs.read` capability (`--allow-read`).

**This is the route a large document must take.** A toke `@(byte)` stores one byte per `i64` slot, so routing a 20 MiB report through `pdf.open` costs 160 MiB of toke array before a glyph is decoded. Reading the container in C costs the file's own size, once.

### pdf.pagecount(doc: $pdfdoc): i64

Pages in the document.

### pdf.pagewidth(doc: $pdfdoc; page: i64): f64
### pdf.pageheight(doc: $pdfdoc; page: i64): f64

The page's `/MediaBox` size in points. You need the height to convert a run's `y` to a top-down coordinate, and assuming A4 is wrong for every US document -- silently.

### pdf.runs(doc: $pdfdoc; page: i64): @($textrun)!$pdferr

The text runs of one page, 1-based, **in content-stream order**. See trap 4.

A page with no text yields zero runs, never an error.

### pdf.pagetext(doc: $pdfdoc; page: i64): $str!$pdferr

One page as a single string, **in reading order**: runs sorted top-to-bottom then left-to-right, grouped into lines by baseline proximity, joined with a space where the horizontal gap exceeds a quarter em, and separated by `\n` between lines.

Baselines within a third of the larger font's size count as the same line, so a superscript or a differently-sized cell does not become its own line.

### pdf.hastext(doc: $pdfdoc; page: i64): bool

`true` when the page yields at least one run with non-whitespace text. Pass `0` for the page to ask about the whole document, which stops at the first page that has text.

This is what routes a document to OCR or away from it.

### pdf.close(doc: $pdfdoc): void

Releases the document and every object in it.

### pdf.lasterr(): $str
### pdf.lasterrkind(): $str

Why the most recent `pdf.*` call failed. `lasterrkind` returns one of `none`, `baddoc`, `encrypted`, `nopage`, `toolarge`, `unsupported`, `io` -- switch on that. `lasterr` is the human-readable sentence.

## Encrypted documents

An encrypted document is **refused**, with kind `encrypted`. It is not decrypted, even when the user password is empty and a viewer would open it without asking: "this file is protected" is the fact the caller needs, and silently opening a protected document is not a decision this module should make on anyone's behalf.

## How objects are found

Not through the cross-reference table. `std.pdf` walks the file from the front, parses every `N G obj`, and keeps the **last** definition of each object number -- then unpacks every `/ObjStm` and indexes its contents too. Trailer dictionaries and `/Type /XRef` stream dictionaries are read only for `/Root` and `/Encrypt`.

The trade, stated plainly:

- It reads files whose xref is wrong, and there are many -- an xref-driven reader returns "not a PDF" for files every viewer opens, which is a wrong answer, not a safe one.
- Classic tables and xref streams are handled identically, because neither is consulted.
- The walk steps **over** stream bodies rather than scanning them, so a `12 0 obj` occurring inside compressed image data cannot invent a phantom object. A naive pattern search would.
- It costs O(file) at open, paid once.
- In a file with incremental updates it takes the last definition in file order. That is correct for real incremental updates, which append.

Unpacking `/ObjStm` is not optional: since PDF 1.5 the catalogue and page tree are routinely packed, and a reader that skips it reports zero pages for most modern output.

## What is not here

**Embedded image extraction** is not implemented (story 135.3 item 5, deliberately). It needs DCTDecode/JPXDecode/CCITTFaxDecode passthrough and a decision about what a toke-side image handle is, and it shares nothing with the text path but the object parser. Half of it would be worse than none.

Also absent: rendering, decryption, OCR, form-field values, annotations, and the predefined CJK CMaps.

## Usage Example

```toke
m=example;
i=io:std.io;
i=s:std.str;
i=f:std.fmt;
i=p:std.pdf;

(* Every column of a statement, with the x that identifies it. *)
f=showrun(r:$textrun):i64{
  let v=mut.f.f64(r.x;1);
  v=s.concat(v;"  ");
  v=s.concat(v;f.f64(r.y;1));
  v=s.concat(v;"  ");
  v=s.concat(v;r.text);
  io.println(v);
  <0
};

f=eachrun(runs:@($textrun)):i64{
  lp(let i=0;i<runs.len;i=i+1){
    showrun(runs.get(i))
  };
  <0
};

f=page(doc:$pdfdoc;n:i64):i64{
  let r=p.runs(doc;n);
  mt r {
    $ok:runs eachrun(runs);
    $err:e io.println(s.concat("page failed: ";p.lasterr()))
  };
  <0
};

f=dump(doc:$pdfdoc):i64{
  io.println(s.concat("pages: ";s.fromint(p.pagecount(doc))));
  if(p.hastext(doc;0)){
    lp(let n=1;n<=p.pagecount(doc);n=n+1){
      page(doc;n)
    }
  }el{
    io.println("no text layer: this document needs OCR")
  };
  p.close(doc);
  <0
};

f=main():i64{
  let d=p.openfile("statement.pdf");
  mt d {
    $ok:doc dump(doc);
    $err:e report()
  };
  <0
};

(* The kind is what to switch on; the message is for a human. *)
f=report():i64{
  let k=p.lasterrkind();
  if(s.eq(k;"encrypted")){
    io.println("this statement is password protected")
  }el{
    io.println(s.concat(s.concat("rejected (";k);s.concat("): ";p.lasterr())))
  };
  <0
};
```

## Limits, in one place

| Constant (`src/stdlib/pdf.h`) | Value | What it bounds |
|---|---|---|
| `TK_PDF_MAX_FILE` | 256 MiB | The whole file; a larger one is refused unread |
| `TK_PDF_MAX_OBJECTS` | 2,000,000 | Distinct object numbers indexed from one file |
| `TK_PDF_MAX_OBJSTM` | 65,536 | Objects packed into one `/ObjStm` |
| `TK_PDF_MAX_PAGES` | 65,536 | Pages, and the page-tree walk -- a `/Kids` cycle is legal bytes |
| `TK_PDF_MAX_DEPTH` | 64 | Array/dictionary nesting while parsing one object |
| `TK_PDF_MAX_FORMDEPTH` | 8 | Form XObject recursion; a form that draws itself is an infinite loop |
| `TK_PDF_MAX_STREAM` | 128 MiB | Bytes any one stream may inflate to |
| `TK_PDF_MAX_CONTENT` | 128 MiB | Concatenated decoded content for one page |
| `TK_PDF_MAX_RUNS` | 200,000 | Text runs from one page |
| `TK_PDF_MAX_RUNBYTES` | 8,192 | UTF-8 bytes in one run |
| `TK_PDF_MAX_CMAP` | 65,536 | Entries in one `/ToUnicode` CMap |
| `TK_PDF_MAX_FONTS` | 4,096 | Distinct fonts cached for one document |
| `TK_PDF_MAX_WIDTHS` | 65,536 | Entries in a `/Widths` or `/W` array |

They are compile-time constants deliberately, for the reason [std.zip](/docs/stdlib/zip) gives: a bound settable from toke is under the control of whichever code path is handling the hostile input.

## See Also

- [ADR-0015](/docs/decisions/ADR-0015/) -- why no C++ library is vendored
- [The 135.3 route note](/docs/decisions/135.3-pdf-extraction-route/) -- what each candidate cost, and what was left out
- [std.xlsx](/docs/stdlib/xlsx) -- the other document format, when the input is a workbook
- [std.zip](/docs/stdlib/zip) -- the container std.xlsx is built on; a PDF is **not** a zip
