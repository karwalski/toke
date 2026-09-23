#!/usr/bin/env bash
# C032_pdf_text_runs.sh — text runs with positions out of a PDF (story 135.3).
#
# std.pdf is a purpose-built C99 extractor, not a vendored engine: ADR-0015
# disqualifies every C++ candidate, the only mostly-C engine is AGPL against
# this repository's Apache-2.0, and there is no permissively-licensed C99
# extractor to find.  The costing is docs/decisions/135.3-pdf-extraction-route.md.
#
# "EXTRACTED SOMETHING" IS TREATED AS FAILURE THROUGHOUT.  Not one assertion
# below is on non-emptiness.  Every text assertion names the exact expected
# string, and every position assertion names the exact point the PRODUCER was
# told to draw at.  That is the whole discipline of this file: a PDF
# extractor's characteristic failure is output that looks like text, and
# nothing but exact comparison tells the difference.
#
# THE FIXTURES COME FROM INDEPENDENT PRODUCERS. reportlab writes the PDFs,
# pikepdf (qpdf) re-writes one of them with object streams, Pillow makes the
# scan image.  None of them shares this parser's misunderstandings, and the
# numbers the width assertions are compared against are reportlab's OWN
# metrics, read out of reportlab at fixture time rather than typed here.
#
# THE TRAPS, AND THE ASSERTION THAT CATCHES EACH:
#
#   1. FONT ENCODING AND WIDTHS. reportlab writes the standard-14 fonts with
#      `/Encoding /WinAnsiEncoding` and NO /Widths array at all — verified as
#      a premise below, not assumed. So every width std.pdf reports comes
#      from its compiled AFM metrics, and a module without them would have to
#      guess. The widths are asserted against reportlab's own stringWidth to
#      three decimal places.
#
#   2. LIGATURES AND NON-ASCII. The embedded-TrueType fixture draws
#      `final office éü — ﬁn`, which carries U+00E9, U+00FC, an em dash and
#      the U+FB01 LATIN SMALL LIGATURE FI. Those reach the content stream as
#      2-byte glyph codes that mean nothing outside the font's /ToUnicode
#      CMap. A reader that treats codes as characters returns mojibake that
#      still looks like text.
#
#   3. READING ORDER IS NOT EMISSION ORDER. outoforder.pdf draws FOURTH,
#      THIRD, SECOND, FIRST in that order — the premise asserts that byte
#      order in the content stream — and the test then requires BOTH that
#      `pdf.runs` returns them in emission order and that `pdf.pagetext`
#      returns them in reading order. Either one alone would let half the
#      implementation be wrong.
#
#   4. THE PAGE TREE MAY NOT BE IN THE FILE BODY. objstm.pdf is table.pdf
#      re-saved by pikepdf with object streams and a cross-reference stream
#      and NO `trailer` keyword — again asserted as a premise. Its ten runs
#      must come out identical to table.pdf's, which is only possible if the
#      /ObjStm contents are unpacked.
#
#   5. ENCRYPTED IS NOT EMPTY. An encrypted statement whose text decodes to
#      noise looks exactly like a statement with no text. It must be refused
#      with the DISTINCT kind "encrypted".
#
#   6. SCANNED IS NOT FAILED. An image-only page must open, report one page,
#      zero runs, and hastext=no — never an error, because the caller's next
#      move (route to OCR) depends on telling those apart.
#
# WHAT IS DELIBERATELY NOT HERE: embedded image extraction (story 135.3
# item 5, not shipped — see the route note), decryption, and OCR.
#
# Story: 135.3

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

# The repo's tkc is a symlink any concurrent `make` relinks (131.39), so a
# caller may pin a resolved binary for the run.
TKC="${TKC:-${REPO_ROOT}/toke}"

PASS=0
FAIL=0

expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label"
        echo "        expected: $want"
        echo "        got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}

fail_only() {
    echo "FAIL $1"
    echo "        $2"
    FAIL=$((FAIL + 1))
}

# The value after "<key>=" on its own line.  Literal prefix match, not a
# regex: keys contain '-' and values contain '|' and '.'.
line() {
    printf '%s\n' "$1" |
        awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); exit }'
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_pdf_XXXXXX)"
FIX="${WORK}/fix"
mkdir -p "${FIX}"
trap 'rm -rf "${WORK}"' EXIT

echo "C032: text runs with positions out of a PDF — encoding, widths, order"
echo "--------------------------------------"

# ════════════════════════════════════════════════════════════════════════
# 0. FIXTURES, FROM INDEPENDENT PRODUCERS
#
# 131.79: a harness failure must announce itself AS a harness failure, here,
# rather than masquerade as a std.pdf failure three hundred lines later.
# ════════════════════════════════════════════════════════════════════════
GEN_LOG="${WORK}/generate.log"

gen_died() {
    echo "FAIL generate-fixtures: $1"
    echo "    HARNESS FAILURE, not a std.pdf failure. The fixture PDFs every"
    echo "    assertion below is compared against were not produced, so"
    echo "    nothing below was tested."
    if [ -s "${GEN_LOG}" ]; then
        echo "    generator output:"
        sed 's/^/        /' "${GEN_LOG}"
    fi
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, $((FAIL + 1)) failed"
    exit 1
}

# reportlab, pikepdf and Pillow are the independent producers.  None is a
# build dependency of this repository, so a checkout without them SKIPS
# rather than fails — but it skips LOUDLY, with a non-zero exit, because a
# silent skip of the only behavioural test for a module is how a module comes
# to be unprotected.
#
# `-P` is not decoration: without it Python puts the script's own directory
# at the front of sys.path, and a stray `math.py` beside it shadows the
# standard library and breaks reportlab's import with a message that points
# nowhere near the real cause.
PY="${TKC_PY:-python3}"
if ! "${PY}" -P -c 'import reportlab, pikepdf, PIL' >/dev/null 2>&1; then
    for cand in /tmp/pdfvenv/bin/python "${HOME}/.venvs/pdf/bin/python"; do
        if [ -x "${cand}" ] && "${cand}" -P -c 'import reportlab, pikepdf, PIL' >/dev/null 2>&1; then
            PY="${cand}"
            break
        fi
    done
fi
if ! "${PY}" -P -c 'import reportlab, pikepdf, PIL' >/dev/null 2>&1; then
    echo "SKIP: python3 lacks reportlab / pikepdf / Pillow — the independent"
    echo "      producers this test's evidence depends on are unavailable."
    echo "      Install them with 'python3 -m pip install reportlab pikepdf"
    echo "      pillow' (a virtualenv is fine; set TKC_PY to its"
    echo "      interpreter). Refusing to substitute hand-written PDF bytes:"
    echo "      a fixture written by the same hand as the parser agrees with"
    echo "      the parser's mistakes, which is the one thing this test"
    echo "      exists to rule out."
    exit 1
fi

if ! "${PY}" -P - "${FIX}" "${WORK}" > "${GEN_LOG}" 2>&1 <<'PY'
import os, re, sys, zlib

from reportlab.pdfgen import canvas
from reportlab.lib.pagesizes import letter
from reportlab.lib import pdfencrypt
from reportlab.pdfbase import pdfmetrics
from reportlab.pdfbase.ttfonts import TTFont
import pikepdf
from PIL import Image, ImageDraw

F, W = sys.argv[1], sys.argv[2]

# ── simple.pdf and flate.pdf: the SAME page, one raw and one Flate ──────
# Two files rather than one so the filter is the only difference between
# them.  Every run asserted for one is asserted identically for the other,
# which is what proves the inflate path rather than merely exercising it.
def simple(path, compress):
    c = canvas.Canvas(path, pagesize=letter, pageCompression=compress)
    c.setFont("Helvetica", 12)
    c.drawString(72, 700, "Opening balance")
    c.drawString(72, 680, "Hello World")
    c.setFont("Times-Roman", 10)
    c.drawString(200, 660, "Times sample")
    c.showPage()
    c.setFont("Helvetica", 12)
    c.drawString(100, 500, "Page two")
    c.showPage()
    c.save()

simple(F + "/simple.pdf", 0)
simple(F + "/flate.pdf", 1)

# ── table.pdf: a statement-shaped grid, which is the case that matters ──
# Four columns at known x, three rows at known y, and a HOLE: the coffee row
# has no Credit and the salary row has no Debit.  A reader that infers
# position from order puts 3200.00 in the Debit column.
COLS = [60, 200, 330, 430]
HEAD = ["Date", "Description", "Debit", "Credit"]
ROWS = [("2026-01-02", "Coffee shop", "4.50", ""),
        ("2026-01-03", "Salary", "", "3200.00")]
c = canvas.Canvas(F + "/table.pdf", pagesize=letter, pageCompression=0)
c.setFont("Helvetica-Bold", 10)
for x, h in zip(COLS, HEAD):
    c.drawString(x, 720, h)
c.setFont("Helvetica", 10)
y = 700
for r in ROWS:
    for x, v in zip(COLS, r):
        if v:
            c.drawString(x, y, v)
    y -= 14
c.showPage()
c.save()

# ── objstm.pdf: table.pdf with object streams and an xref stream ────────
pdf = pikepdf.open(F + "/table.pdf")
pdf.save(F + "/objstm.pdf",
         object_stream_mode=pikepdf.ObjectStreamMode.generate,
         compress_streams=True)
pdf.close()

# ── outoforder.pdf: emitted bottom-up, right-to-left ────────────────────
c = canvas.Canvas(F + "/outoforder.pdf", pagesize=letter, pageCompression=0)
c.setFont("Helvetica", 12)
for x, yy, t in [(300, 600, "FOURTH"), (72, 600, "THIRD"),
                 (300, 700, "SECOND"), (72, 700, "FIRST")]:
    c.drawString(x, yy, t)
c.showPage()
c.save()

# ── encrypted.pdf ──────────────────────────────────────────────────────
enc = pdfencrypt.StandardEncryption("userpw", "ownerpw", canPrint=0)
c = canvas.Canvas(F + "/encrypted.pdf", pagesize=letter, encrypt=enc)
c.setFont("Helvetica", 12)
c.drawString(72, 700, "Secret balance")
c.showPage()
c.save()

# ── scanned.pdf: an image, and no text operator anywhere ───────────────
img = Image.new("RGB", (400, 120), (255, 255, 255))
ImageDraw.Draw(img).text((10, 40), "SCANNED INVOICE 12345", fill=(0, 0, 0))
img.save(W + "/scan.png")
c = canvas.Canvas(F + "/scanned.pdf", pagesize=letter, pageCompression=1)
c.drawImage(W + "/scan.png", 72, 600, width=400, height=120)
c.showPage()
c.save()

# ── embedded.pdf: a subset TrueType, 2-byte codes, a /ToUnicode CMap ───
# The only route to meaning for these codes is the CMap, and the string
# carries a ligature and three non-ASCII characters so that a reader which
# skipped the CMap cannot accidentally be right.
EMB_TEXT = "final office éü — ﬁn"
TTF_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "/usr/share/fonts/dejavu/DejaVuSans.ttf",
    "/Library/Fonts/Arial Unicode.ttf",
]
ttf = next((p for p in TTF_CANDIDATES if os.path.exists(p)), None)
emb_width = ""
if ttf:
    pdfmetrics.registerFont(TTFont("EmbFont", ttf))
    c = canvas.Canvas(F + "/embedded.pdf", pagesize=letter, pageCompression=1)
    c.setFont("EmbFont", 14)
    c.drawString(72, 700, EMB_TEXT)
    c.showPage()
    c.save()
    emb_width = "%.3f" % pdfmetrics.stringWidth(EMB_TEXT, "EmbFont", 14)

# ── cid.pdf: a composite (Type0) font with a PREDEFINED CMap and NO
#    /ToUnicode.  This is the case where the file contains NO INFORMATION
#    about what its codes mean that std.pdf implements, and the correct
#    behaviour is to say so LOUDLY rather than to guess.  reportlab's
#    UnicodeCIDFont writes exactly that: /Encoding /UniJIS-UCS2-H, two-byte
#    codes, no ToUnicode stream anywhere.
CID_TEXT = "\u3053\u3093\u306b\u3061\u306f ABC"
cid_ok = False
try:
    from reportlab.pdfbase.cidfonts import UnicodeCIDFont
    pdfmetrics.registerFont(UnicodeCIDFont("HeiseiMin-W3"))
    c = canvas.Canvas(F + "/cid.pdf", pagesize=letter, pageCompression=0)
    c.setFont("HeiseiMin-W3", 14)
    c.drawString(72, 700, CID_TEXT)
    c.showPage()
    c.save()
    cid_ok = True
except Exception as exc:                      # noqa: BLE001 - reported, not hidden
    print("cid fixture unavailable:", type(exc).__name__, exc)

# ── a file that is not a PDF at all ────────────────────────────────────
open(F + "/notapdf.pdf", "wb").write(b"this is not a pdf, not even slightly\n" * 8)

# ════════════════════════════════════════════════════════════════════════
# THE PREMISES, READ BACK OUT OF THE FILES
#
# Each of these is a fact the assertions below DEPEND on. Reading them out
# of the produced bytes rather than restating them from memory is what makes
# a later reportlab or pikepdf change announce itself here instead of
# quietly turning a real test into a decorative one.
# ════════════════════════════════════════════════════════════════════════
def raw(name):
    return open(F + "/" + name, "rb").read()

def page_content(name, page=0):
    """Page `page`'s content stream, DECODED BY pikepdf.

    Decoded by an independent library on purpose.  Scanning the raw bytes
    for `stream`/`endstream` and inflating by hand is what a naive reader
    does, it fails on exactly the compressed files this premise is about,
    and a premise that is itself wrong is worse than no premise.
    """
    with pikepdf.open(F + "/" + name) as pdf:
        pg = pdf.pages[page]
        c = pg.Contents
        if isinstance(c, pikepdf.Array):
            return b"".join(bytes(x.read_bytes()) for x in c)
        return bytes(c.read_bytes())

def shows_text(content):
    """Does this content stream contain a text-SHOWING operator?

    `BT` is not enough: reportlab emits empty `BT ... ET` pairs that set a
    font and show nothing, so an image-only page still contains `BT`.  The
    question that matters is whether any Tj/TJ/'/" is executed.
    """
    return bool(re.search(rb"(?m)(?:\)|\])\s*(?:Tj|TJ)|'|\"", content))

ref = {}

# The Helvetica font object, and what it does NOT carry.
with pikepdf.open(F + "/simple.pdf") as _p:
    _f = _p.pages[0].Resources.Font
    _helv = next((v for v in _f.values()
                  if str(v.get("/BaseFont", "")) == "/Helvetica"), None)
    ref["HELV_FONT_FOUND"] = "yes" if _helv is not None else "no"
    ref["HELV_HAS_WIDTHS"] = "yes" if (_helv is not None and "/Widths" in _helv) else "no"
    ref["HELV_ENCODING"] = ("WinAnsi"
                            if (_helv is not None and
                                str(_helv.get("/Encoding", "")) == "/WinAnsiEncoding")
                            else "other")

ref["SIMPLE_HAS_FLATE"] = "yes" if b"/FlateDecode" in raw("simple.pdf") else "no"
ref["FLATE_HAS_FLATE"] = "yes" if b"/FlateDecode" in raw("flate.pdf") else "no"

# The two files' page-1 content streams must be byte-identical once decoded,
# or "flate matches simple" proves nothing about the filter.
ref["CONTENT_IDENTICAL"] = ("yes" if page_content("simple.pdf") == page_content("flate.pdf")
                            else "no")

# Emission order in outoforder.pdf: FOURTH must come FIRST in the bytes.
ooo = page_content("outoforder.pdf")
pos = {t: ooo.find(t.encode()) for t in ("FIRST", "SECOND", "THIRD", "FOURTH")}
ref["OOO_EMISSION_ORDER"] = ",".join(
    t for t in sorted(pos, key=lambda k: pos[k]) if pos[t] >= 0)

objstm_raw = raw("objstm.pdf")
ref["OBJSTM_HAS_OBJSTM"] = "yes" if b"/ObjStm" in objstm_raw else "no"
ref["OBJSTM_HAS_XREFSTM"] = "yes" if b"/XRef" in objstm_raw else "no"
ref["OBJSTM_HAS_TRAILER_KW"] = "yes" if re.search(rb"[\r\n]trailer", objstm_raw) else "no"

ref["ENC_HAS_ENCRYPT"] = "yes" if b"/Encrypt" in raw("encrypted.pdf") else "no"

scanned_cs = page_content("scanned.pdf")
ref["SCAN_SHOWS_TEXT"] = "yes" if shows_text(scanned_cs) else "no"
ref["SCAN_HAS_IMAGE"] = "yes" if b"/Image" in raw("scanned.pdf") else "no"
# The control: the same predicate must say YES for a page that does show
# text, or "no text operator" would be a claim about a broken detector.
ref["SIMPLE_SHOWS_TEXT"] = "yes" if shows_text(page_content("simple.pdf")) else "no"

ref["EMB_PRESENT"] = "yes" if ttf else "no"
if ttf:
    with pikepdf.open(F + "/embedded.pdf") as _p:
        _fonts = dict(_p.pages[0].Resources.Font)
        _sub = next((v for v in _fonts.values()
                     if str(v.get("/BaseFont", "")).find("+") >= 0), None)
        ref["EMB_SUBTYPE"] = str(_sub.Subtype) if _sub is not None else "none"
        ref["EMB_HAS_TOUNICODE"] = "yes" if (_sub is not None and "/ToUnicode" in _sub) else "no"
        ref["EMB_IS_SUBSET"] = "yes" if _sub is not None else "no"
ref["EMB_TEXT"] = EMB_TEXT
ref["EMB_W"] = emb_width
ref["EMB_HAS_LIGATURE"] = "yes" if "\ufb01" in EMB_TEXT else "no"

ref["CID_PRESENT"] = "yes" if cid_ok else "no"
if cid_ok:
    with pikepdf.open(F + "/cid.pdf") as _p:
        _t0 = next((v for v in _p.pages[0].Resources.Font.values()
                    if str(v.get("/Subtype", "")) == "/Type0"), None)
        ref["CID_IS_TYPE0"] = "yes" if _t0 is not None else "no"
        ref["CID_ENCODING"] = str(_t0.get("/Encoding", "none")) if _t0 is not None else "none"
        ref["CID_HAS_TOUNICODE"] = "yes" if (_t0 is not None and "/ToUnicode" in _t0) else "no"
    ref["CID_NCHARS"] = str(len(CID_TEXT))
    # What std.pdf MUST return: one U+FFFD per code, because the file says
    # nothing about what its codes mean and a guess would be plausible
    # output that is wrong.
    ref["CID_EXPECT"] = "\ufffd" * len(CID_TEXT)
    # The width std.pdf must report, and why it is NOT the true width.
    # /W is indexed by CID; under /UniJIS-UCS2-H the content-stream code is
    # a UCS-2 unit, and only that CMap says which CID it becomes.  With no
    # CMap, indexing /W by the code returns a real width belonging to a
    # DIFFERENT glyph -- a plausible wrong answer.  The defensible answer is
    # /DW for every glyph: uniformly approximate rather than selectively
    # wrong.  CID_TRUE_W is what reportlab, which HAS the CMap, computes;
    # the test asserts the two differ so this stays a recorded
    # approximation rather than a coincidence nobody rechecked.
    with pikepdf.open(F + "/cid.pdf") as _p:
        _t0w = next(v for v in _p.pages[0].Resources.Font.values()
                    if str(v.get("/Subtype", "")) == "/Type0")
        _dw = float(_t0w.DescendantFonts[0].get("/DW", 1000))
    ref["CID_DW"] = "%g" % _dw
    ref["CID_W"] = "%.3f" % (len(CID_TEXT) * _dw / 1000.0 * 14)
    ref["CID_TRUE_W"] = "%.3f" % pdfmetrics.stringWidth(CID_TEXT, "HeiseiMin-W3", 14)

# ── the widths the assertions compare against: reportlab's OWN metrics ──
def w(text, font, size):
    return "%.3f" % pdfmetrics.stringWidth(text, font, size)

ref["W_OPENING"] = w("Opening balance", "Helvetica", 12)
ref["W_HELLO"] = w("Hello World", "Helvetica", 12)
ref["W_TIMES"] = w("Times sample", "Times-Roman", 10)
for i, h in enumerate(HEAD):
    ref["W_HEAD%d" % i] = w(h, "Helvetica-Bold", 10)
ref["W_DATE1"] = w("2026-01-02", "Helvetica", 10)
ref["W_COFFEE"] = w("Coffee shop", "Helvetica", 10)
ref["W_450"] = w("4.50", "Helvetica", 10)
ref["W_SALARY"] = w("Salary", "Helvetica", 10)
ref["W_3200"] = w("3200.00", "Helvetica", 10)
ref["W_FIRST"] = w("FIRST", "Helvetica", 12)
ref["W_FOURTH"] = w("FOURTH", "Helvetica", 12)
ref["W_PAGETWO"] = w("Page two", "Helvetica", 12)

# The embedded font's em box, from the font's OWN descriptor rather than
# from this module's defaults: (ascent - descent) x size.  Asserting it
# against a number std.pdf also computed would prove nothing.
if ttf:
    face = pdfmetrics.getFont("EmbFont").face
    ref["EMB_H"] = "%.3f" % ((face.ascent - face.descent) / 1000.0 * 14)

with open(W + "/ref.sh", "w") as fh:
    for k, v in ref.items():
        fh.write("%s='%s'\n" % (k, str(v).replace("'", "'\\''")))

print("fixtures ok:", sorted(os.listdir(F)))
PY
then
    gen_died "the fixture generator exited non-zero"
fi

[ -s "${WORK}/ref.sh" ] || gen_died "no reference file was written"
for _n in HELV_HAS_WIDTHS CONTENT_IDENTICAL OOO_EMISSION_ORDER \
          OBJSTM_HAS_OBJSTM ENC_HAS_ENCRYPT W_OPENING W_3200; do
    grep -q "^${_n}=" "${WORK}/ref.sh" || gen_died "reference file does not set ${_n}"
done
# shellcheck disable=SC1091
. "${WORK}/ref.sh" || gen_died "the reference file could not be sourced"

for _f in simple.pdf flate.pdf table.pdf objstm.pdf outoforder.pdf \
          encrypted.pdf scanned.pdf notapdf.pdf; do
    [ -s "${FIX}/${_f}" ] || gen_died "fixture ${_f} was not written"
done

# ════════════════════════════════════════════════════════════════════════
# 1. THE PREMISES, ASSERTED AS FACT AND NOT ASSUMED
# ════════════════════════════════════════════════════════════════════════
expect "premise-helvetica-font-object-found" "${HELV_FONT_FOUND}" "yes"
# The load-bearing one: with no /Widths in the file, every width std.pdf
# reports must come from its own compiled AFM metrics. If reportlab ever
# starts writing /Widths, the width assertions below would pass for a
# different reason and this line says so.
expect "premise-standard14-font-has-no-widths-array" "${HELV_HAS_WIDTHS}" "no"
expect "premise-standard14-font-is-winansi" "${HELV_ENCODING}" "WinAnsi"
expect "premise-simple-content-is-not-compressed" "${SIMPLE_HAS_FLATE}" "no"
expect "premise-flate-content-is-compressed" "${FLATE_HAS_FLATE}" "yes"
expect "premise-both-files-hold-identical-content" "${CONTENT_IDENTICAL}" "yes"
# Emission order is the REVERSE of reading order, so a reader that returns
# either one cannot be mistaken for a reader that returns the other.
expect "premise-emission-order-is-not-reading-order" \
    "${OOO_EMISSION_ORDER}" "FOURTH,THIRD,SECOND,FIRST"
expect "premise-objstm-packs-objects" "${OBJSTM_HAS_OBJSTM}" "yes"
expect "premise-objstm-uses-xref-stream" "${OBJSTM_HAS_XREFSTM}" "yes"
# No `trailer` keyword at all: /Root is reachable only through the xref
# stream's dictionary, and the page tree only by unpacking the object stream.
expect "premise-objstm-has-no-trailer-keyword" "${OBJSTM_HAS_TRAILER_KW}" "no"
expect "premise-encrypted-file-has-encrypt-dict" "${ENC_HAS_ENCRYPT}" "yes"
# `BT` alone is not the question: reportlab emits empty BT/ET pairs, so an
# image-only page still contains one. What must be absent is a text-SHOWING
# operator, and the same predicate is asserted to say "yes" for a page that
# does show text so that "no" is a finding and not a broken detector.
expect "premise-scanned-page-shows-no-text" "${SCAN_SHOWS_TEXT}" "no"
expect "premise-text-page-does-show-text" "${SIMPLE_SHOWS_TEXT}" "yes"
expect "premise-scanned-page-does-have-an-image" "${SCAN_HAS_IMAGE}" "yes"
expect "premise-embedded-string-carries-a-ligature" "${EMB_HAS_LIGATURE}" "yes"

# ════════════════════════════════════════════════════════════════════════
# 2. LINK. A program importing std.pdf AND NOTHING ELSE must build.
#    136.33 registered six modules' glue under the wrong name; that defect
#    is invisible unless the module is the sole import, because any other
#    import drags the glue in.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/link.tk" <<'TKEOF'
m=pdflink;
i=p:std.pdf;

f=main():i64{
  let r=p.openfile("/nonexistent/none.pdf");
  mt r { $ok:d p.close(d); $err:e 0 };
  <0
};
TKEOF
if "${TKC}" --out "${WORK}/link" "${WORK}/link.tk" > "${WORK}/link.log" 2>&1; then
    expect "link-pdf-alone" "ok" "ok"
else
    fail_only "link-pdf-alone" "a program importing only std.pdf did not build"
    sed 's/^/        /' "${WORK}/link.log"
fi

# ════════════════════════════════════════════════════════════════════════
# 3. The consumer.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/pdf.tk" <<'TKEOF'
m=pdfconsumer;
i=io:std.io;
i=s:std.str;
i=f:std.fmt;
i=env:std.env;
i=fl:std.file;
i=p:std.pdf;

f=fix(name:str):str{
  <s.concat(s.concat(env.getor("PDFFIX";"/tmp/pdffix");"/");name)
};

(* text|page|x|y|width|height|fontsize -- EVERY declared field of $textrun.
   A .tki/glue slot mismatch still compiles and then reads whatever sits at
   the offset (127.86), and five of these seven slots carry an f64 BIT
   PATTERN, so every field is printed and compared. *)
f=runline(r:$textrun):str{
  let v=mut.r.text;
  v=s.concat(v;"|"); v=s.concat(v;s.fromint(r.page));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.x;2));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.y;2));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.width;3));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.height;3));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.fontsize;2));
  <v
};

f=each(tag:str;runs:@($textrun)):i64{
  io.println(s.concat(s.concat(tag;"-nruns=");s.fromint(runs.len)));
  lp(let i=0;i<runs.len;i=i+1){
    io.println(s.concat(s.concat(s.concat(s.concat(tag;"-r");s.fromint(i));"=");runline(runs.get(i))))
  };
  <0
};

f=body(tag:str;doc:$pdfdoc;page:i64):i64{
  io.println(s.concat(s.concat(tag;"-pages=");s.fromint(p.pagecount(doc))));
  io.println(s.concat(s.concat(s.concat(s.concat(tag;"-size=");f.f64(p.pagewidth(doc;page);2));"x");f.f64(p.pageheight(doc;page);2)));
  io.println(s.concat(s.concat(tag;"-hastext=");if(p.hastext(doc;page)){"yes"}el{"no"}));
  io.println(s.concat(s.concat(tag;"-hastextdoc=");if(p.hastext(doc;0)){"yes"}el{"no"}));
  let rr=p.runs(doc;page);
  mt rr {
    $ok:runs each(tag;runs);
    $err:e io.println(s.concat(s.concat(tag;"-nruns=ERR|");p.lasterrkind()))
  };
  let t=p.pagetext(doc;page);
  mt t {
    $ok:txt io.println(s.concat(s.concat(tag;"-text=");s.replace(txt;"\n";"/")));
    $err:e io.println(s.concat(s.concat(tag;"-text=ERR|");p.lasterrkind()))
  };
  p.close(doc);
  <0
};

f=dump(tag:str;file:str;page:i64):i64{
  let d=p.openfile(fix(file));
  mt d {
    $ok:doc body(tag;doc;page);
    $err:e io.println(s.concat(s.concat(s.concat(s.concat(tag;"-open=ERR|");p.lasterrkind());"|");p.lasterr()))
  };
  <0
};

(* The in-memory path: pdf.open over a @(byte), which must agree with
   pdf.openfile cell for cell. *)
f=dumpmem(tag:str;file:str;page:i64):i64{
  let b=fl.readbytes(fix(file));
  mt b {
    $ok:bytes memopen(tag;bytes;page);
    $err:e io.println(s.concat(tag;"-open=READERR"))
  };
  <0
};

f=memopen(tag:str;bytes:@(byte);page:i64):i64{
  let d=p.open(bytes);
  mt d {
    $ok:doc body(tag;doc;page);
    $err:e io.println(s.concat(s.concat(tag;"-open=ERR|");p.lasterrkind()))
  };
  <0
};

(* A document that must be REFUSED, and the KIND the reader gave.
   Prints ACCEPTED when it opened, which is the failure. *)
f=mustreject(label:str;file:str):i64{
  let d=p.openfile(fix(file));
  mt d {
    $ok:doc rejectfail(label;doc);
    $err:e io.println(s.concat(s.concat(s.concat("reject-";label);"=");p.lasterrkind()))
  };
  <0
};

f=rejectfail(label:str;doc:$pdfdoc):i64{
  io.println(s.concat(s.concat("reject-";label);"=ACCEPTED"));
  p.close(doc);
  <0
};

f=badpage():i64{
  let d=p.openfile(fix("simple.pdf"));
  mt d {
    $ok:doc badpagebody(doc);
    $err:e io.println("badpage=OPENERR")
  };
  <0
};

f=badpagebody(doc:$pdfdoc):i64{
  let r=p.runs(doc;99);
  mt r {
    $ok:runs io.println(s.concat("badpage=ACCEPTED|";s.fromint(runs.len)));
    $err:e io.println(s.concat("badpage=";p.lasterrkind()))
  };
  p.close(doc);
  <0
};

f=main():i64{
  dump("simple";"simple.pdf";1);
  dump("simple2";"simple.pdf";2);
  dump("flate";"flate.pdf";1);
  dump("mem";"simple.pdf";1);
  dumpmem("mem";"simple.pdf";1);
  dump("table";"table.pdf";1);
  dump("objstm";"objstm.pdf";1);
  dump("ooo";"outoforder.pdf";1);
  dump("scan";"scanned.pdf";1);
  dump("emb";"embedded.pdf";1);
  dump("cid";"cid.pdf";1);
  mustreject("encrypted";"encrypted.pdf");
  mustreject("notapdf";"notapdf.pdf");
  badpage();
  <0
};
TKEOF

if ! "${TKC}" --out "${WORK}/pdfrun" "${WORK}/pdf.tk" > "${WORK}/build.log" 2>&1; then
    fail_only "build-consumer" "the std.pdf consumer did not build"
    sed 's/^/        /' "${WORK}/build.log"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"
    exit 1
fi

OUT="$(PDFFIX="${FIX}" "${WORK}/pdfrun" --allow-read --allow-env 2>&1)"
if [ -z "${OUT}" ]; then
    fail_only "run-consumer" "the consumer produced no output at all"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"
    exit 1
fi

# ════════════════════════════════════════════════════════════════════════
# 4. simple.pdf — exact text at exact positions, with exact widths.
#
#    The widths are reportlab's own stringWidth values, so this asserts the
#    compiled AFM metrics against the metrics the producer laid the page out
#    with. A module that guessed 0.5 em per glyph would report 90.000 for
#    "Opening balance" instead of 90.720 and every one of these would fail.
# ════════════════════════════════════════════════════════════════════════
expect "simple-pagecount"  "$(line "${OUT}" simple-pages)"  "2"
expect "simple-pagesize"   "$(line "${OUT}" simple-size)"   "612.00x792.00"
expect "simple-hastext"    "$(line "${OUT}" simple-hastext)" "yes"
expect "simple-nruns"      "$(line "${OUT}" simple-nruns)"  "3"
expect "simple-run0" "$(line "${OUT}" simple-r0)" \
    "Opening balance|1|72.00|700.00|${W_OPENING}|12.000|12.00"
expect "simple-run1" "$(line "${OUT}" simple-r1)" \
    "Hello World|1|72.00|680.00|${W_HELLO}|12.000|12.00"
expect "simple-run2" "$(line "${OUT}" simple-r2)" \
    "Times sample|1|200.00|660.00|${W_TIMES}|10.000|10.00"
expect "simple-pagetext" "$(line "${OUT}" simple-text)" \
    "Opening balance/Hello World/Times sample"

# Page 2 is a different page, and per-page access must show that rather than
# re-reading page 1.
expect "simple-page2-nruns" "$(line "${OUT}" simple2-nruns)" "1"
expect "simple-page2-run0" "$(line "${OUT}" simple2-r0)" \
    "Page two|2|100.00|500.00|${W_PAGETWO}|12.000|12.00"
# .page is the field that says which page a run came from; a module that
# stamped every run with 1 would pass every other assertion above.
expect "simple-page2-run-carries-page-2" \
    "$(printf '%s' "$(line "${OUT}" simple2-r0)" | cut -d'|' -f2)" "2"

# ════════════════════════════════════════════════════════════════════════
# 5. flate.pdf — the SAME page through FlateDecode.
#
#    The premise above proved both files hold byte-identical content once
#    decoded, so any difference here is the inflate path's fault and nothing
#    else's.
# ════════════════════════════════════════════════════════════════════════
expect "flate-nruns" "$(line "${OUT}" flate-nruns)" "3"
for _i in 0 1 2; do
    expect "flate-run${_i}-matches-uncompressed" \
        "$(line "${OUT}" "flate-r${_i}")" "$(line "${OUT}" "simple-r${_i}")"
done
expect "flate-pagetext-matches" "$(line "${OUT}" flate-text)" "$(line "${OUT}" simple-text)"

# ════════════════════════════════════════════════════════════════════════
# 6. pdf.open over @(byte) must agree with pdf.openfile.
# ════════════════════════════════════════════════════════════════════════
expect "mem-nruns" "$(line "${OUT}" mem-nruns)" "3"
for _i in 0 1 2; do
    expect "mem-run${_i}-matches-openfile" \
        "$(line "${OUT}" "mem-r${_i}")" "$(line "${OUT}" "simple-r${_i}")"
done

# ════════════════════════════════════════════════════════════════════════
# 7. table.pdf — the case the story is actually about.
#
#    Ten runs, four columns, and two HOLES: the coffee row has no Credit and
#    the salary row has no Debit. A reader that inferred column from order
#    would put 3200.00 at x=330 (the Debit column) instead of x=430, and the
#    text would still look perfectly plausible. The x coordinate is what
#    catches it.
# ════════════════════════════════════════════════════════════════════════
expect "table-nruns" "$(line "${OUT}" table-nruns)" "10"
expect "table-head-date"   "$(line "${OUT}" table-r0)" "Date|1|60.00|720.00|${W_HEAD0}|10.000|10.00"
expect "table-head-desc"   "$(line "${OUT}" table-r1)" "Description|1|200.00|720.00|${W_HEAD1}|10.000|10.00"
expect "table-head-debit"  "$(line "${OUT}" table-r2)" "Debit|1|330.00|720.00|${W_HEAD2}|10.000|10.00"
expect "table-head-credit" "$(line "${OUT}" table-r3)" "Credit|1|430.00|720.00|${W_HEAD3}|10.000|10.00"
expect "table-row1-date"   "$(line "${OUT}" table-r4)" "2026-01-02|1|60.00|700.00|${W_DATE1}|10.000|10.00"
expect "table-row1-desc"   "$(line "${OUT}" table-r5)" "Coffee shop|1|200.00|700.00|${W_COFFEE}|10.000|10.00"
expect "table-row1-debit"  "$(line "${OUT}" table-r6)" "4.50|1|330.00|700.00|${W_450}|10.000|10.00"
expect "table-row2-date"   "$(line "${OUT}" table-r7)" "2026-01-03|1|60.00|686.00|${W_DATE1}|10.000|10.00"
expect "table-row2-desc"   "$(line "${OUT}" table-r8)" "Salary|1|200.00|686.00|${W_SALARY}|10.000|10.00"
# THE ONE THAT MATTERS: the salary amount is in the CREDIT column (x=430),
# not the Debit column it would land in if position came from order.
expect "table-row2-credit-is-in-the-credit-column" \
    "$(line "${OUT}" table-r9)" "3200.00|1|430.00|686.00|${W_3200}|10.000|10.00"
expect "table-pagetext" "$(line "${OUT}" table-text)" \
    "Date Description Debit Credit/2026-01-02 Coffee shop 4.50/2026-01-03 Salary 3200.00"

# ════════════════════════════════════════════════════════════════════════
# 8. objstm.pdf — the same page, packed into object streams.
#
#    No `trailer` keyword, an xref stream, and the page tree inside an
#    /ObjStm. Every run must come out identical to table.pdf's, which is
#    impossible without unpacking the object stream — a reader that skips
#    that step reports zero pages, which is what most modern producers'
#    output would do to it.
# ════════════════════════════════════════════════════════════════════════
expect "objstm-pagecount" "$(line "${OUT}" objstm-pages)" "1"
expect "objstm-nruns" "$(line "${OUT}" objstm-nruns)" "10"
for _i in 0 1 2 3 4 5 6 7 8 9; do
    expect "objstm-run${_i}-matches-table" \
        "$(line "${OUT}" "objstm-r${_i}")" "$(line "${OUT}" "table-r${_i}")"
done

# ════════════════════════════════════════════════════════════════════════
# 9. outoforder.pdf — both halves of the order question.
#
#    The premise proved the producer emitted FOURTH, THIRD, SECOND, FIRST in
#    that byte order. `pdf.runs` must return that order (it is the file's own
#    order and nothing is gained by hiding it), and `pdf.pagetext` must
#    return reading order (which is derivable only from the positions — the
#    argument for having positions at all).
# ════════════════════════════════════════════════════════════════════════
expect "ooo-nruns" "$(line "${OUT}" ooo-nruns)" "4"
expect "ooo-run0-is-the-last-one-read" "$(line "${OUT}" ooo-r0)" \
    "FOURTH|1|300.00|600.00|${W_FOURTH}|12.000|12.00"
expect "ooo-run0-text" "$(printf '%s' "$(line "${OUT}" ooo-r0)" | cut -d'|' -f1)" "FOURTH"
expect "ooo-run1-text" "$(printf '%s' "$(line "${OUT}" ooo-r1)" | cut -d'|' -f1)" "THIRD"
expect "ooo-run2-text" "$(printf '%s' "$(line "${OUT}" ooo-r2)" | cut -d'|' -f1)" "SECOND"
expect "ooo-run3-text" "$(printf '%s' "$(line "${OUT}" ooo-r3)" | cut -d'|' -f1)" "FIRST"
expect "ooo-run3-position" "$(line "${OUT}" ooo-r3)" \
    "FIRST|1|72.00|700.00|${W_FIRST}|12.000|12.00"
expect "ooo-pagetext-is-reading-order" "$(line "${OUT}" ooo-text)" \
    "FIRST SECOND/THIRD FOURTH"

# ════════════════════════════════════════════════════════════════════════
# 10. scanned.pdf — an image-only page. NOT an error, and NOT text.
#
#     This is the signal that routes a document to OCR. Extraction is exact
#     and OCR is probabilistic, so a caller that cannot tell "no text layer"
#     from "failed to read" either pays OCR's error rate on documents that
#     never needed it, or silently reports an empty statement.
# ════════════════════════════════════════════════════════════════════════
expect "scan-opens" "$(line "${OUT}" scan-pages)" "1"
expect "scan-hastext-is-no" "$(line "${OUT}" scan-hastext)" "no"
expect "scan-hastext-document-wide-is-no" "$(line "${OUT}" scan-hastextdoc)" "no"
expect "scan-nruns-is-zero-not-an-error" "$(line "${OUT}" scan-nruns)" "0"
expect "scan-pagetext-is-empty-not-an-error" "$(line "${OUT}" scan-text)" ""

# ════════════════════════════════════════════════════════════════════════
# 11. embedded.pdf — a subset TrueType with 2-byte codes.
#
#     The content stream holds glyph indices, not characters. Their only
#     meaning is the /ToUnicode CMap, and the string carries é, ü, an em
#     dash and the ﬁ ligature so a reader that skipped the CMap cannot be
#     accidentally right. Skipped where no TrueType font was found on the
#     machine — and the skip is REPORTED, not silent.
# ════════════════════════════════════════════════════════════════════════
if [ "${EMB_PRESENT}" = "yes" ]; then
    expect "premise-embedded-font-has-tounicode" "${EMB_HAS_TOUNICODE}" "yes"
    # reportlab embeds this as a SUBSET TrueType (an `AAAAAA+` BaseFont),
    # not as a composite font. That is worth pinning: the subset prefix is
    # what makes the built-in standard-14 metrics inapplicable, so the
    # widths here come from the file's own /Widths array and the characters
    # from its /ToUnicode — two paths neither of the standard-14 fixtures
    # reaches.
    expect "premise-embedded-font-is-a-subset" "${EMB_IS_SUBSET}" "yes"
    expect "premise-embedded-font-subtype" "${EMB_SUBTYPE}" "/TrueType"
    expect "emb-nruns" "$(line "${OUT}" emb-nruns)" "1"
    expect "emb-text-is-exact-including-the-ligature" \
        "$(printf '%s' "$(line "${OUT}" emb-r0)" | cut -d'|' -f1)" "${EMB_TEXT}"
    # The em box comes from the font's OWN /FontDescriptor, not from this
    # module's 1.0-em fallback; EMB_H is computed from reportlab's copy of
    # the same face.
    expect "emb-position-and-width" "$(line "${OUT}" emb-r0)" \
        "${EMB_TEXT}|1|72.00|700.00|${EMB_W}|${EMB_H}|14.00"
    expect "emb-pagetext" "$(line "${OUT}" emb-text)" "${EMB_TEXT}"
else
    echo "NOTE emb: no TrueType font found on this machine; the embedded-font"
    echo "     case did not run. The ligature and CMap paths are UNTESTED here."
fi

# ════════════════════════════════════════════════════════════════════════
# 12. cid.pdf — a composite font whose codes MEAN NOTHING in the file.
#
#     reportlab's UnicodeCIDFont writes /Type0 with the predefined CMap
#     /UniJIS-UCS2-H and NO /ToUnicode stream. std.pdf does not implement
#     the predefined CMaps, so there is genuinely no information available
#     about what these two-byte codes mean — and a full renderer is no
#     better off except by heuristic.
#
#     The requirement is therefore NOT that the text come out right. It is
#     that the failure be LOUD: one U+FFFD per code, so the output is
#     visibly wrong rather than plausibly wrong. And `hastext` must still
#     say YES, because the page HAS a text layer — OCR is not the remedy
#     for a missing /ToUnicode, and routing this document to OCR would be
#     the wrong call.
# ════════════════════════════════════════════════════════════════════════
if [ "${CID_PRESENT}" = "yes" ]; then
    expect "premise-cid-font-is-composite" "${CID_IS_TYPE0}" "yes"
    expect "premise-cid-font-uses-a-predefined-cmap" "${CID_ENCODING}" "/UniJIS-UCS2-H"
    expect "premise-cid-font-has-no-tounicode" "${CID_HAS_TOUNICODE}" "no"
    expect "cid-nruns" "$(line "${OUT}" cid-nruns)" "1"
    expect "cid-unmappable-codes-are-loudly-replaced" \
        "$(printf '%s' "$(line "${OUT}" cid-r0)" | cut -d'|' -f1)" "${CID_EXPECT}"
    # The approximation is recorded, not assumed away: /DW x 9 is NOT the
    # true width, and this says so before asserting it.
    expect "premise-cid-default-width-differs-from-the-true-width" \
        "$([ "${CID_W}" != "${CID_TRUE_W}" ] && echo yes || echo no)" "yes"
    expect "cid-width-falls-back-to-dw-rather-than-misindexing-w" \
        "$(line "${OUT}" cid-r0)" \
        "${CID_EXPECT}|1|72.00|700.00|${CID_W}|$(printf '%s' "$(line "${OUT}" cid-r0)" | cut -d'|' -f6)|14.00"
    expect "cid-position-is-exact" \
        "$(printf '%s' "$(line "${OUT}" cid-r0)" | cut -d'|' -f3,4,7)" "72.00|700.00|14.00"
    # A text layer with no usable mapping is still a text layer.
    expect "cid-hastext-is-yes-not-a-scan" "$(line "${OUT}" cid-hastext)" "yes"
else
    echo "NOTE cid: reportlab's CID fonts are unavailable here; the composite"
    echo "     two-byte-code path is UNTESTED in this run."
fi

# ════════════════════════════════════════════════════════════════════════
# 13. What must be REFUSED, and with WHICH kind.
#
#     "encrypted" is a distinct answer from "baddoc" on purpose: a
#     password-protected statement that came back as "no text" would read as
#     a statement with nothing in it.
# ════════════════════════════════════════════════════════════════════════
expect "reject-encrypted-is-its-own-kind" "$(line "${OUT}" reject-encrypted)" "encrypted"
expect "reject-notapdf" "$(line "${OUT}" reject-notapdf)" "baddoc"
expect "badpage-is-nopage" "$(line "${OUT}" badpage)" "nopage"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
