#!/usr/bin/env python3
"""gen_pdf_fontdata.py — generate src/stdlib/pdffont.c for std.pdf (story 135.3).

The table this emits is the part of std.pdf that must NOT be written from
memory.  It carries two kinds of fact, and getting either subtly wrong
produces the exact failure mode story 135.3 warns about — output that looks
plausible and is wrong:

  1. glyph name -> Unicode, for the three base encodings a simple font can
     name (StandardEncoding, WinAnsiEncoding, MacRomanEncoding) and for every
     name reachable through an /Encoding /Differences array.
  2. glyph name -> advance width, for the standard-14 fonts.  A standard-14
     font is allowed to omit /Widths entirely — reportlab and most producers
     do — so without these numbers every run's width is a guess, and a guess
     is what makes a two-column layout collapse into one.

Both come from independent sources that were written to interoperate with
Acrobat, not with this parser:

  widths     reportlab's AFM-derived reportlab.pdfbase._fontdata
  encodings  pypdf's _codecs tables (code -> character)
  names      reportlab's _fontdata.encodings (code -> glyph name)

Run it from the repository root:

    /tmp/pdfvenv/bin/python scripts/gen_pdf_fontdata.py > src/stdlib/pdffont.c

It is NOT run by the build.  The generated file is committed, because the
build must not need reportlab; this script exists so the numbers in it can be
re-derived and diffed rather than trusted.
"""

import sys

from reportlab.pdfbase import _fontdata as fd
import pypdf._codecs as codecs

# The six width vectors that are actually distinct and actually used.
# Courier* is 600 for every glyph (checked below, not assumed) and is handled
# as a special case in C.  Symbol and ZapfDingbats carry their own built-in
# encodings and are out of scope for this module.
FONTS = [
    "Helvetica",        # Helvetica-Oblique shares these widths
    "Helvetica-Bold",   # Helvetica-BoldOblique shares these widths
    "Times-Roman",
    "Times-Bold",
    "Times-Italic",
    "Times-BoldItalic",
]

ENCODINGS = ["StandardEncoding", "WinAnsiEncoding", "MacRomanEncoding"]


def check_invariants() -> None:
    """Assert here what the C code assumes, so a reportlab change is loud."""
    for f in ("Courier", "Courier-Bold", "Courier-Oblique", "Courier-BoldOblique"):
        ws = set(fd.widthsByFontGlyph[f].values())
        assert ws == {600}, f"{f} is not monospaced at 600: {sorted(ws)}"
    for a, b in (("Helvetica", "Helvetica-Oblique"),
                 ("Helvetica-Bold", "Helvetica-BoldOblique")):
        assert fd.widthsByFontGlyph[a] == fd.widthsByFontGlyph[b], \
            f"{a} and {b} no longer share a width vector"
    for e in ENCODINGS:
        assert len(fd.encodings[e]) == 256, e


def main() -> int:
    check_invariants()

    # code -> glyph name, per base encoding
    enc_names = {e: fd.encodings[e] for e in ENCODINGS}
    # code -> unicode character, per base encoding (pypdf, independent source)
    enc_uni = {
        "StandardEncoding": codecs._std_encoding,
        "WinAnsiEncoding": codecs._win_encoding,
        "MacRomanEncoding": codecs._mac_encoding,
    }

    # Union of every glyph name reachable through a base encoding, plus every
    # name the standard-14 width vectors know (a /Differences array may name a
    # glyph no base encoding uses).
    names: set[str] = set()
    for e in ENCODINGS:
        for n in enc_names[e]:
            if n and n != ".notdef":
                names.add(n)
    for f in FONTS:
        names.update(fd.widthsByFontGlyph[f].keys())
    names.discard(".notdef")

    # name -> unicode, resolved from the encodings that use the name.  A name
    # appearing at code c in encoding E means U+xxxx == enc_uni[E][c].
    name_uni: dict[str, int] = {}
    conflicts: list[str] = []
    for e in ENCODINGS:
        for c, n in enumerate(enc_names[e]):
            if not n or n == ".notdef":
                continue
            ch = enc_uni[e][c]
            if ch is None or ch == "":
                continue
            u = ord(ch[0]) if len(ch) == 1 else None
            if u is None:
                continue
            if n in name_uni and name_uni[n] != u:
                conflicts.append(f"{n}: U+{name_uni[n]:04X} vs U+{u:04X} in {e}")
            name_uni.setdefault(n, u)

    # pypdf's Adobe Glyph List closes the gap for names no base encoding
    # places (reachable only through /Differences).
    for n in sorted(names):
        if n not in name_uni:
            ch = codecs.adobe_glyphs.get(n)
            if ch and len(ch) == 1:
                name_uni[n] = ord(ch)

    if conflicts:
        print("/* CONFLICTS: " + "; ".join(conflicts) + " */", file=sys.stderr)

    rows = sorted(names)

    out = sys.stdout.write
    out('/*\n')
    out(' * pdffont.c — GENERATED.  Do not edit by hand.\n')
    out(' *\n')
    out(' *   scripts/gen_pdf_fontdata.py > src/stdlib/pdffont.c\n')
    out(' *\n')
    out(' * Two facts per glyph name, both from sources written to interoperate with\n')
    out(' * Acrobat rather than with this parser:\n')
    out(' *\n')
    out(' *   uni  the Unicode code point, from pypdf\'s encoding tables and the\n')
    out(' *        Adobe Glyph List.  0 means "no single code point".\n')
    out(' *   w    the advance width in 1/1000 em for the six distinct standard-14\n')
    out(' *        width vectors, from reportlab\'s AFM-derived metrics:\n')
    out(' *\n')
    out(' *          0 Helvetica (and -Oblique)   3 Times-Bold\n')
    out(' *          1 Helvetica-Bold (and -BO)   4 Times-Italic\n')
    out(' *          2 Times-Roman                5 Times-BoldItalic\n')
    out(' *\n')
    out(' *        Courier* is 600 for every glyph and is not tabulated; the\n')
    out(' *        generator asserts that rather than assuming it.  0 means the\n')
    out(' *        font has no such glyph.\n')
    out(' *\n')
    out(' * WHY THE WIDTHS MATTER AT ALL.  A standard-14 font may omit /Widths, and\n')
    out(' * producers routinely do.  Without these numbers every run\'s width is a\n')
    out(' * guess — and a wrong width is how a two-column layout is read as one.\n')
    out(' *\n')
    out(' * Story: 135.3\n')
    out(' */\n\n')
    out('#include "pdffont.h"\n\n')
    out('#include <string.h>\n\n')
    out('/* Sorted by name so lookup is a binary search. */\n')
    out('const TkPdfGlyph tk_pdf_glyphs[] = {\n')
    for n in rows:
        u = name_uni.get(n, 0)
        w = [int(fd.widthsByFontGlyph[f].get(n, 0)) for f in FONTS]
        out('    {"%s", 0x%04X, {%s}},\n'
            % (n, u, ", ".join("%4d" % x for x in w)))
    out('};\n\n')
    out('const unsigned tk_pdf_glyph_count = %d;\n\n' % len(rows))

    # code -> glyph-name index, per base encoding.  -1 for unused codes.
    idx = {n: i for i, n in enumerate(rows)}
    for cname, ename in (("std", "StandardEncoding"),
                         ("win", "WinAnsiEncoding"),
                         ("mac", "MacRomanEncoding")):
        out('/* %s: code -> index into tk_pdf_glyphs, -1 where unused. */\n' % ename)
        out('const short tk_pdf_enc_%s[256] = {\n' % cname)
        vals = []
        for c in range(256):
            n = enc_names[ename][c]
            vals.append(idx[n] if n and n in idx else -1)
        for i in range(0, 256, 8):
            out('    ' + ', '.join('%4d' % v for v in vals[i:i + 8]) + ',\n')
        out('};\n\n')
    return 0


if __name__ == "__main__":
    sys.exit(main())
