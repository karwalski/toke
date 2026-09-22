---
title: std.xlsx
slug: xlsx
section: reference/stdlib
order: 44
---

**Status: Implemented** -- C runtime backing (story 135.4), built on [std.zip](/docs/stdlib/zip). **No second library is vendored for this.**

`std.xlsx` reads **cell values** out of an XLSX workbook.

An XLSX is a zip of XML, so with `std.zip` already in place this is a focused scanner over five parts: `xl/workbook.xml`, `xl/_rels/workbook.xml.rels`, `xl/sharedStrings.xml`, `xl/styles.xml`, and the worksheet parts. ADR-0015 leaves the toolchain C99 only, so vendoring a spreadsheet library was never an option -- and it would not have helped, because the format's difficulty is not in the XML.

## Scope: values only

No formula evaluation, no styling, no charts, no pivot tables. A formula cell yields its **cached** `<v>`, which is what a data-import consumer wants; evaluating formulae is an unbounded project with no bearing on reading a bank statement.

The one apparent exception is not one: `xl/styles.xml` **is** read, but only to recover each cell's number-format id. Telling a date from a number is not presentation -- it is the cell's type, and nothing in the cell itself says which it is.

## The three things that catch every first implementation

Each produces a **plausible wrong answer** rather than a failure. That is why they are called out here, and why `test/conform/C031_xlsx_cell_values.sh` asserts on resolved cell values rather than on "it parsed".

### 1. Cell text is usually an index, not text

A text cell normally carries `t="s"` and a **number**: the position of its text in `xl/sharedStrings.xml`. A parser reading only the worksheet returns `4` where the column header says `Description`.

This one is worse than it looks, because it is easy to write a fixture that hides it. `openpyxl` -- the obvious independent encoder -- writes every text cell **inline** (`t="inlineStr"`) and emits no shared-strings part at all. A parser that has never heard of the pool reads openpyxl's output perfectly and fails on everything Excel has ever saved. C031 therefore tests both encodings, and builds the pooled fixture itself because openpyxl will not.

`std.xlsx` resolves the index and keeps the index: `.value` is the text, `.raw` is the number that pointed at it.

Two details inside the pool bite as well, and both are handled: one entry can be **many** `<t>` elements (rich text splits a single string across a run per formatting change), and `<rPh>` holds phonetic annotation that is *not* part of the string.

### 2. Dates are serial numbers, and the 1900 leap-year bug is reproduced

A date cell holds a serial number counted from an epoch the workbook chooses. In the **1900** system that count is wrong on purpose: Lotus 1-2-3 treated 1900 as a leap year, Excel copied the defect for import compatibility, and forty years of spreadsheets have been written through it since.

**Serial 60 is 1900-02-29** -- a day that never happened. So the 1900 system has two epochs, not one:

| Serial | Counted from | Example |
|---|---|---|
| 1 .. 59 | 1899-12-31 | 59 -> 1900-02-28 |
| 60 | -- | the phantom day, 1900-02-29 |
| 61 .. | 1899-12-30 | 61 -> 1900-03-01 |

"Fixing" this means using one epoch throughout. That is right for every modern date and **wrong by one day for everything on or before 1900-02-28** -- and the error is silent, because 1899-12-31 is as plausible a date as 1900-01-01. We are decoding someone else's encoding, so the encoding is what we follow.

The **1904** system (`<workbookPr date1904="1"/>`, the old Mac default) counts from 1904-01-01 as serial 0 and has no such defect. Applying the bug there is the other half of getting this wrong: the same serial 59 is `1900-02-28` in a 1900 workbook and `1904-02-29` -- a real leap day -- in a 1904 one.

Ask which system a workbook uses with `xlsx.datesystem`, and convert a serial directly with `xlsx.datefromserial`.

### 3. Rows and columns are sparse

A worksheet stores only the cells that have something in them, and each carries its own reference:

```xml
<row r="7"><c r="A7" t="s"><v>0</v></c><c r="C7"><v>12.5</v></c></row>
```

Row 7 has two cells and the second one is column **C**. Counting cells left to right puts C7's value under B -- for that row only, so the file looks right everywhere the rows are full and is wrong exactly where a gap is, which in a bank statement is the optional-reference column. Rows are sparse the same way: `<row r="9">` may follow `<row r="4">`.

`std.xlsx` never infers position from order. Every cell is placed at the position its own `r=` parses to, and **`xlsx.rows` returns a rectangle** with the holes filled in as `empty` cells. A consumer indexing `rows.get(i).get(j)` is therefore right by construction rather than by remembering to check -- and every cell still carries `.ref`, `.row` and `.col`, so one that cross-checks the placement can.

## Types

### $xlsxbook

An opaque handle. Release it with `xlsx.close`; everything derived from it is dangling afterwards.

### $xlsxcell

| Field | Type | Meaning |
|---|---|---|
| `ref` | `$str` | The cell reference, `"C7"`. Always present, including for a filled-in hole |
| `row` | `i64` | 1-based row |
| `col` | `i64` | 1-based column; 3 for `C7` |
| `celltype` | `$str` | `empty`, `str`, `num`, `date`, `bool` or `err` |
| `raw` | `$str` | What was in the file before resolution: the pool **index** for a shared string, the **serial** for a date |
| `value` | `$str` | The resolved value as text |

`raw` is kept for two reasons: a resolution this module gets wrong is undiagnosable without it, and a consumer that wants the serial rather than the rendered date should not have to re-read the file to get it.

`value` is text for every kind because one struct field must carry all six. The numeric kinds pass through **verbatim** rather than being reformatted, so `str.tofloat` round-trips them exactly.

`celltype` is a closed, stable set -- switching on it is the intended use:

It is spelled `celltype` and not `kind` for a reason worth knowing: a `.tki` type field named `kind` silently **truncates its own type's field list**. The export-record scanners in `src/llvm.c` delimit one record from the next with `strstr(p + 6, "\"kind\"")`, which matches the *value* `"kind"` of a `"name"` key as readily as the `"kind"` key itself, so every field after it is lost -- measured with `kind` fourth of six, `tkc --check` accepted `.raw` and `.value` while `tkc --out` rejected both as non-existent. That is a compiler defect and is filed as one. `celltype` is also the word the consumer's own request uses ("raw string, type, and resolved value").

| `celltype` | `value` holds |
|---|---|
| `empty` | `""` -- the cell is absent from the file, or present with no content |
| `str` | The text, resolved from the pool or taken inline |
| `num` | The number verbatim, as the file spells it |
| `date` | ISO-8601: `YYYY-MM-DD`, `YYYY-MM-DDTHH:MM:SS` when the serial has a fraction, or `HH:MM:SS` for a time-only format |
| `bool` | `"true"` or `"false"` |
| `err` | Excel's own error text, `#DIV/0!` and friends |

### $xlsxerr

The error side of every fallible call: `$badworkbook`, `$badpart`, `$nosheet`, `$toolarge`, `$io`.

## Functions

### xlsx.open(data: @(byte)): $xlsxbook!$xlsxerr

Opens a workbook held in memory. The archive goes through `std.zip`, so every rule there applies -- traversal, entry count, size and ratio caps included.

`workbook.xml`, its relationships, `sharedStrings.xml` and `styles.xml` are read here. Worksheets are **not**: a workbook with fifty sheets costs one sheet's memory when one sheet is wanted.

### xlsx.openfile(path: $str): $xlsxbook!$xlsxerr

Reads `path` and opens it. Requires the `fs.read` capability (`--allow-read`).

**This is the route a large workbook must take.** Not because of the zip, but because a toke `@(byte)` stores one byte per `i64` slot: routing a 40 MiB workbook through `xlsx.open` costs 320 MiB of toke array before a single cell is read. Reading the container in C costs the file's own size, once. See *Large workbooks* below.

### xlsx.sheets(wb: $xlsxbook): @($str)

Sheet display names in tab order -- `xl/workbook.xml`'s order.

Neither the order nor the part name is derivable from the other: `sheet1.xml` is not necessarily the first tab, and nothing requires the parts to be named `sheetN.xml` at all. `std.xlsx` resolves each sheet's part through `r:id` and `xl/_rels/workbook.xml.rels`, falling back to the conventional name only when there are no relationships to read. Assuming otherwise reads the right cells from the wrong sheet, which is a data error dressed as a success.

### xlsx.rows(wb: $xlsxbook; sheet: $str): @(@($xlsxcell))!$xlsxerr

Parses one sheet by **display name** into a rectangle: `nrows x ncols`, every position occupied, holes filled in as `empty` cells. See trap 3.

An empty sheet yields zero rows, never an error.

### xlsx.datesystem(wb: $xlsxbook): $str

`"1900"` or `"1904"`. It decides how every serial in the workbook reads, so a consumer that has to guess which one it got is in the position this module exists to remove it from.

### xlsx.datefromserial(serial: $str; system: $str): $str

The date mapping on its own. `serial` is the decimal text of a serial exactly as a `<v>` spells it; `system` is `"1904"` for the 1904 system and anything else for 1900. Returns `""` for text that is not a serial.

Exposed because the leap-year rule is the part of this module most likely to be quietly wrong, and it is the only way to ask about **serial 60** -- no fixture can contain 1900-02-29, because no date library has such a date.

### xlsx.close(wb: $xlsxbook): void

Releases the workbook and its archive.

### xlsx.lasterr(): $str

Why the most recent `xlsx.*` call failed, or `""` if none has.

The same workaround `zip.lasterr` is, for the same reason: the compiled error-union ABI lowers `T!E` to a null check, so the `$err` arm binds nothing a consumer can read, and "the workbook was rejected" would otherwise be indistinguishable from "sheet 'Ledger' is not in this workbook". A rejection that came from the archive rather than the workbook is passed through **verbatim from `zip.lasterr`**, so the rule that fired is still the one reported.

## Usage Example

```toke
m=example;
i=io:std.io;
i=s:std.str;
i=x:std.xlsx;

f=showrow(row:@($xlsxcell)):i64{
  let out=mut."";
  lp(let c=0;c<row.len;c=c+1){
    let cell=row.get(c);
    if(c>0){ out=s.concat(out;"\t") };
    out=s.concat(out;cell.value)
  };
  io.println(out);
  <0
};

f=dump(b:$xlsxbook):i64{
  let names=x.sheets(b);
  io.println(s.concat("date system: ";x.datesystem(b)));
  let r=x.rows(b;names.get(0));
  mt r {
    $ok:rows eachrow(rows);
    $err:e io.println(s.concat("sheet failed: ";x.lasterr()))
  };
  x.close(b);
  <0
};

f=eachrow(rows:@(@($xlsxcell))):i64{
  lp(let i=0;i<rows.len;i=i+1){
    showrow(rows.get(i))
  };
  <0
};

f=main():i64{
  let w=x.openfile("statement.xlsx");
  mt w {
    $ok:b dump(b);
    $err:e io.println(s.concat("rejected: ";x.lasterr()))
  };
  <0
};
```

## Large workbooks

Measured, on workbooks written by openpyxl with eight columns per row:

| Cells | File | Uncompressed XML | Parse | Peak RSS |
|---|---|---|---|---|
| 200,000 | 936 KB | 8.1 MB | 0.09 s | 59 MiB |
| 480,000 | 2.2 MB | 19.7 MB | 0.22 s | 138 MiB |
| 1,040,000 | 4.8 MB | 43.3 MB | refused | -- |

**`TK_XLSX_MAX_CELLS` is what a large workbook meets first, not the archive caps.** The 1,040,000-cell workbook is refused with `xlsx: sheet cell cap exceeded (rows x columns)` while its 43 MB of XML is still comfortably inside `std.zip`'s 64 MiB cumulative-uncompressed ceiling -- at roughly 41 bytes of XML per populated cell, that ceiling would not bind until about 1.6 million cells. So the archive caps are the *second* line of defence here, and the cell cap is the one to raise if a real input needs more.

**The toke side is the reason `xlsx.openfile` exists.** `xlsx.open` takes `@(byte)`, which costs eight bytes per byte, so a 5 MB workbook routed through it would want 40 MB of toke array before a cell is read -- and anything over 64 MiB could not be read into one at all. `xlsx.openfile` reads and parses the container in C; only the resolved cells cross into toke.

**[`file.readrange`](/docs/stdlib/file) (story 135.12) is not on this path.** `xlsx.openfile` does not route through it and does not need to: `std.zip` reads the archive with the C library, which already seeks. The bounded read is what a *toke-level* consumer needs to window a large file by hand; it is not what makes a large workbook readable here.

## Limits, in one place

| Constant (`src/stdlib/xlsx.h`) | Value | What it bounds |
|---|---|---|
| `TK_XLSX_MAX_CELLS` | 1,000,000 | Cells materialised for one sheet, **holes included** |
| `TK_XLSX_MAX_SHARED` | 1,000,000 | Entries in the shared-string pool |
| `TK_XLSX_MAX_COL` | 16384 | Highest column reference (`XFD`, the format's own limit) |
| `TK_XLSX_MAX_ROW` | 1,048,576 | Highest row reference (the format's own limit) |
| `TK_XLSX_MAX_XF` | 65536 | Entries in `cellXfs` |
| `TK_XLSX_MAX_SHEETS` | 4096 | Sheets in one workbook |

The cell cap counts the **rectangle**, not the populated cells, because that is what gets allocated: a lone cell at `XFD1048576` would otherwise ask for seventeen billion of them. One million covers a 50,000-row statement twenty columns wide.

They are compile-time constants deliberately, for the reason `std.zip` gives: a bound settable from toke is under the control of whichever code path is handling the hostile input.

## See Also

- [std.zip](/docs/stdlib/zip) -- the container, and every archive rule that applies here
- [std.str](/docs/stdlib/str) -- `str.tofloat` for a `num` cell's value
- [std.csv](/docs/stdlib/csv) -- the simpler format, when the input is not a workbook
