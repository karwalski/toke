#!/usr/bin/env bash
# C033_ocr_text_runs.sh — recognised text runs with positions (story 135.6).
#
# std.ocr binds the operating system's own recogniser (macOS Vision) from a
# plain C99 translation unit through objc_msgSend, the way src/stdlib/webview.c
# binds WebKit.  ADR-0015 refuses vendoring Tesseract, so the alternatives were
# the platform engine or story 135.8's standalone component.  The costing and
# the probe measurements are docs/decisions/135.6-platform-ocr-route.md.
#
# "RECOGNISED SOMETHING" IS TREATED AS FAILURE THROUGHOUT.  Not one text
# assertion below is on non-emptiness.  Every one names the exact expected
# string.  For OCR that discipline matters more than it did for std.pdf, not
# less: a recogniser's characteristic failure is confident, well-formed,
# wrong output, and nothing but exact comparison tells it from a real reading.
#
# THE FIXTURES COME FROM AN INDEPENDENT PRODUCER.  Pillow draws every image,
# with a system TrueType face, and the geometry assertions are compared
# against PILLOW'S OWN textbbox metrics read out at fixture time rather than
# constants typed here.  Pillow is not Apple and does not share Vision's
# misunderstandings, which is the whole point of using it.
#
# THE TRAPS, AND THE ASSERTION THAT CATCHES EACH:
#
#   1. CONFIDENCE IS NOT A RELIABILITY SIGNAL, AND THIS FILE PROVES IT.
#      `confusable.png` reads `Illlll1I0O`.  Vision returns `|||||1100` --
#      flatly wrong -- at confidence EXACTLY 1.0.  The assertion is that
#      pair: the wrong string AND the full confidence.  Story 135.6's brief
#      asks for a case the recogniser gets wrong with "the confidence
#      reflecting it"; the measured answer is that it does not, and pinning
#      that here is what makes a future OS which fixes it announce itself
#      instead of passing silently.
#
#   2. EMPTY IS NOT UNAVAILABLE IS NOT FAILED.  Three different outcomes that
#      all look like "no text came back":
#        - a blank page      -> SUCCESS, zero runs, kind "none"
#        - no recogniser     -> the $err arm, kind "noengine"
#        - unreadable bytes  -> the $err arm, kind "badimage"
#      A module that collapses these tells a caller a scanned statement is
#      empty.  That is the defect class std.pdf refuses with a distinct
#      $encrypted, and requirement 5 of this story is exactly it.  Case 7
#      asserts all five kinds by name.
#
#   3. THE $ocrrun SLOTS MUST MATCH THE .tki.  Seven slots, five of them an
#      f64 BIT PATTERN.  A glue that writes (int64_t)v where it should write
#      the bit pattern yields 1 where the page says 1.0, which compiles,
#      runs and prints a plausible number (127.86).  Every field of every run
#      is printed and compared.
#
#   4. COORDINATES ARE std.pdf's, NOT THE RASTER CONVENTION.  Origin
#      bottom-left, y up, unit = source pixels, so a consumer doing OCR
#      fallback sorts $ocrrun and $textrun with the identical comparison.
#      Get the origin wrong and the lines come out in reverse order while
#      every string is still correct.  Case 5 asserts each run's box against
#      the band Pillow drew it in, and asserts the top-to-bottom ordering.
#
#   5. THE NON-macOS PATH IS UNBUILT ON EVERY PLATFORM.  `make build-all`
#      cross-compiles SRCS, and a stdlib module .c file is not in SRCS --
#      tkc compiles it at program link time, for the host only.  So the #else
#      branch of ocr.c is never built by any gate.  Case 8 builds it here, by
#      compiling ocr.c with -U__APPLE__, and asserts every entry point fails
#      "noengine".  It found a real one: clear_err() was unused off Apple and
#      the file did not compile under -Werror.
#
# WHAT IS DELIBERATELY NOT HERE: per-glyph boxes and per-field confidence
# (story 135.8 -- see the route note for why the platform cannot provide
# them), multi-column reading-order reconstruction, and Windows, which is
# NOENGINE by choice rather than stubbed with plausible values.
#
# Story: 135.6

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

# Geometry, with an explicit tolerance in source pixels.  A recogniser's box
# is its own estimate of where the ink is, so it cannot equal the producer's
# layout box exactly -- but the text bands in these fixtures are 70px apart,
# so a tolerance of 8px cannot confuse one line with another, and the
# assertion stays a real one.
GEOM_TOL=8
near() {
    local label="$1" actual="$2" want="$3"
    local ok
    ok=$(awk -v a="$actual" -v w="$want" -v t="${GEOM_TOL}" \
             'BEGIN { d = a - w; if (d < 0) d = -d; print (d <= t) ? "y" : "n" }')
    if [ "$ok" = "y" ]; then
        echo "PASS $label (${actual} vs ${want}, tol ${GEOM_TOL})"
        PASS=$((PASS + 1))
    else
        echo "FAIL $label"
        echo "        expected: ${want} +/- ${GEOM_TOL}"
        echo "        got:      ${actual}"
        FAIL=$((FAIL + 1))
    fi
}

fail_only() {
    echo "FAIL $1"
    echo "        $2"
    FAIL=$((FAIL + 1))
}

# The value after "<key>=" on its own line.  Literal prefix match, not a
# regex: values contain '|' and '.' and ','.
line() {
    printf '%s\n' "$1" |
        awk -v k="$2=" 'index($0, k) == 1 { print substr($0, length(k) + 1); exit }'
}

# One '|'-separated field of a printed run.  Runs print their numeric fields
# first precisely so these indices are stable; see runline() below.
field() {
    printf '%s\n' "$1" | awk -F'|' -v n="$2" '{ print $n }'
}

# The recognised text: everything after the sixth delimiter.  It may itself
# contain '|' -- the confusable fixture's answer is literally `|||||1100` --
# so it can only be taken as a remainder, never as a field.
runtext() {
    printf '%s\n' "$1" | cut -d'|' -f7-
}

# Field indices in a printed run (text is the remainder, not a field).
F_PAGE=1; F_X=2; F_Y=3; F_W=4; F_H=5; F_CONF=6

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_ocr_XXXXXX)"
FIX="${WORK}/fix"
mkdir -p "${FIX}"
trap 'rm -rf "${WORK}"' EXIT

echo "C033: recognised text runs with positions — exactness, kinds, confidence"
echo "--------------------------------------"

# ════════════════════════════════════════════════════════════════════════
# 0. PLATFORM GATE
#
# std.ocr binds a platform recogniser and there is none on bare Linux.  That
# is the accepted trade in the story row, not a defect, so a non-macOS host
# SKIPS -- but it skips LOUDLY, with a non-zero exit, because a silent skip
# of the only behavioural test for a module is how a module comes to be
# believed working when it is not (131.79).
# ════════════════════════════════════════════════════════════════════════
if [ "$(uname -s)" != "Darwin" ]; then
    echo "SKIP: std.ocr has no engine on $(uname -s); macOS Vision only."
    echo "      Case 8 (the no-engine path) would still be meaningful here and"
    echo "      is the part to run first if this suite is ever ported."
    exit 1
fi

# ════════════════════════════════════════════════════════════════════════
# 1. FIXTURES, FROM AN INDEPENDENT PRODUCER
#
# 131.79: a harness failure must announce itself AS a harness failure, here,
# rather than masquerade as a std.ocr failure three hundred lines later.
# ════════════════════════════════════════════════════════════════════════
GEN_LOG="${WORK}/generate.log"
FACTS="${WORK}/facts.sh"

gen_died() {
    echo "FAIL generate-fixtures: $1"
    echo "    HARNESS FAILURE, not a std.ocr failure. The fixture images every"
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

if ! python3 -c "import PIL" >/dev/null 2>&1; then
    echo "SKIP: Pillow is not installed; it draws every fixture in this suite."
    echo "      Pillow is not a build dependency of this repository. Install it"
    echo "      with 'python3 -m pip install Pillow' to run these assertions."
    exit 1
fi

python3 - "${FIX}" "${FACTS}" > "${GEN_LOG}" 2>&1 <<'PYEOF'
import os
import sys

from PIL import Image, ImageDraw, ImageFont

FIX, FACTS = sys.argv[1], sys.argv[2]

# A real system face, not Pillow's bitmap default: the default font is 11px
# and Vision refuses it, which would make every assertion below vacuous.
FONT_CANDIDATES = [
    "/System/Library/Fonts/Supplemental/Arial.ttf",
    "/System/Library/Fonts/Supplemental/Helvetica.ttf",
    "/Library/Fonts/Arial.ttf",
]
face = next((p for p in FONT_CANDIDATES if os.path.exists(p)), None)
if face is None:
    print("no scalable system font found; cannot draw a legible fixture")
    sys.exit(2)

W, H = 900, 300
LINES = ["ACME BANK", "Balance 1,234.56 GBP", "Ref 998877"]
font = ImageFont.truetype(face, 34)

facts = {}

# ── statement.png: three lines of known text at known positions ────────
img = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(img)
for i, t in enumerate(LINES):
    d.text((30, 30 + i * 70), t, fill="black", font=font)
img.save(FIX + "/statement.png")

# The SAME pixels as a greyscale JPEG, to prove the module is reading the
# image rather than the container: a different codec, a different decoder
# path through ImageIO, and the recognised text must be identical.
grey = Image.open(FIX + "/statement.png").convert("L")
grey.save(FIX + "/statement.jpg", "JPEG", quality=95)

# ...and as RAW GREYSCALE BYTES, which is the composition point with
# std.image: greyscale/crop/threshold there, recognise here, no re-encode.
open(FIX + "/statement.raw", "wb").write(grey.tobytes())
facts["RAW_W"], facts["RAW_H"], facts["RAW_CH"] = W, H, 1
facts["RAW_LEN"] = len(grey.tobytes())

# ── blank.png: white, and PROVABLY white ───────────────────────────────
Image.new("RGB", (W, H), "white").save(FIX + "/blank.png")

# ── tiny.png: below the engine's usable size ───────────────────────────
Image.new("RGB", (12, 12), "white").save(FIX + "/tiny.png")

# ── confusable.png: the case the recogniser gets WRONG ─────────────────
# Capital I, five lower-case L, digit one, capital I, digit zero, capital O.
# Chosen because every glyph has a near-identical twin in this face, so a
# recogniser cannot be accidentally right, and because a bank statement is
# full of exactly this: reference codes with no dictionary to fall back on.
CONFUSABLE = "I" + "l" * 5 + "1" + "I" + "0" + "O"
ci = Image.new("RGB", (700, 140), "white")
ImageDraw.Draw(ci).text((20, 45), CONFUSABLE, fill="black",
                        font=ImageFont.truetype(face, 44))
ci.save(FIX + "/confusable.png")
facts["CONFUSABLE"] = CONFUSABLE

# ── notanimage.png: a .png extension over bytes that are not a PNG ─────
open(FIX + "/notanimage.png", "wb").write(b"this is not a png, not even slightly\n" * 12)

# ════════════════════════════════════════════════════════════════════════
# THE PREMISES, READ BACK OUT OF THE PRODUCED FILES
#
# Each is a fact the assertions depend on. Reading them out of the bytes
# that were actually written -- rather than restating them from memory --
# is what makes a later Pillow or font change announce itself here instead
# of quietly turning a real test into a decorative one.
# ════════════════════════════════════════════════════════════════════════

def ink_rows(path, thresh=128):
    """Rows (top-left origin) containing at least one dark pixel."""
    im = Image.open(path).convert("L")
    px = im.load()
    rows = []
    for y in range(im.height):
        for x in range(im.width):
            if px[x, y] < thresh:
                rows.append(y)
                break
    return rows, im.size

# P1 — statement.png really is a 900x300 PNG.
st = Image.open(FIX + "/statement.png")
facts["P_ST_FORMAT"] = st.format
facts["P_ST_SIZE"] = "%dx%d" % st.size

# P2 — it really has ink, in THREE separated horizontal bands.  This is the
# premise that the image contains what we think it contains: not "a file
# exists" but "there are marks on it, in three groups, where text was drawn".
rows, _ = ink_rows(FIX + "/statement.png")
bands = []
for y in rows:
    if bands and y <= bands[-1][1] + 1:
        bands[-1][1] = y
    else:
        bands.append([y, y])
facts["P_ST_INKBANDS"] = len(bands)
facts["P_ST_INKROWS"] = len(rows)

# P3 — blank.png has ZERO dark pixels.  Without this, "blank yields no runs"
# proves nothing: it could be a blank result from a non-blank page.
brows, _ = ink_rows(FIX + "/blank.png")
facts["P_BLANK_INKROWS"] = len(brows)

# P4 — confusable.png carries the exact code points intended, and has ink.
crows, csize = ink_rows(FIX + "/confusable.png")
facts["P_CONF_INKROWS"] = len(crows)
facts["P_CONF_CODEPOINTS"] = " ".join("%04X" % ord(c) for c in CONFUSABLE)

# P5 — notanimage.png is not decodable as an image BY AN INDEPENDENT READER.
try:
    Image.open(FIX + "/notanimage.png").load()
    facts["P_NOTIMG_DECODES"] = "yes"
except Exception:
    facts["P_NOTIMG_DECODES"] = "no"

# P6 — the JPEG is a different container holding the same picture.
jp = Image.open(FIX + "/statement.jpg")
facts["P_JPG_FORMAT"] = jp.format
facts["P_JPG_SIZE"] = "%dx%d" % jp.size

# P7 — PILLOW'S OWN LAYOUT METRICS for each line, converted to std.pdf's
# convention (origin bottom-left, y up).  These are what the geometry
# assertions compare against: the producer's numbers, not this file's.
for i, t in enumerate(LINES):
    l, t_, r, b = d.textbbox((30, 30 + i * 70), t, font=font)
    facts["P_L%d_TEXT" % i] = t
    facts["P_L%d_X" % i] = "%.2f" % l
    facts["P_L%d_W" % i] = "%.2f" % (r - l)
    facts["P_L%d_Y" % i] = "%.2f" % (H - b)       # bottom edge, y-up
    facts["P_L%d_H" % i] = "%.2f" % (b - t_)

facts["P_EXPECT_TEXT"] = "/".join(LINES)          # ocr.text, newlines as '/'
facts["P_NLINES"] = len(LINES)

with open(FACTS, "w") as fh:
    for k, v in facts.items():
        fh.write("%s='%s'\n" % (k, str(v).replace("'", "'\\''")))
print("fixtures written")
PYEOF

[ -f "${FACTS}" ] || gen_died "the generator produced no facts file"
# shellcheck disable=SC1090
. "${FACTS}"
[ -n "${P_ST_SIZE:-}" ] || gen_died "the facts file is missing P_ST_SIZE"

# ════════════════════════════════════════════════════════════════════════
# 2. THE PREMISES THEMSELVES
#
# Asserted before anything is asked of std.ocr. If a premise is wrong,
# every assertion that depends on it is meaningless, and it is better to
# learn that here than to read a green result that measured nothing.
# ════════════════════════════════════════════════════════════════════════
expect "premise-statement-is-png"        "${P_ST_FORMAT}"      "PNG"
expect "premise-statement-size"          "${P_ST_SIZE}"        "900x300"
expect "premise-statement-has-3-bands"   "${P_ST_INKBANDS}"    "3"
expect "premise-blank-has-no-ink"        "${P_BLANK_INKROWS}"  "0"
expect "premise-notanimage-undecodable"  "${P_NOTIMG_DECODES}" "no"
expect "premise-jpeg-is-jpeg"            "${P_JPG_FORMAT}"     "JPEG"
expect "premise-jpeg-same-size"          "${P_JPG_SIZE}"       "900x300"
expect "premise-confusable-codepoints"   "${P_CONF_CODEPOINTS}" \
       "0049 006C 006C 006C 006C 006C 0031 0049 0030 004F"
if [ "${P_ST_INKROWS}" -gt 30 ]; then
    echo "PASS premise-statement-has-ink (${P_ST_INKROWS} inked rows)"
    PASS=$((PASS + 1))
else
    fail_only "premise-statement-has-ink" "only ${P_ST_INKROWS} inked rows"
fi
if [ "${P_CONF_INKROWS}" -gt 10 ]; then
    echo "PASS premise-confusable-has-ink (${P_CONF_INKROWS} inked rows)"
    PASS=$((PASS + 1))
else
    fail_only "premise-confusable-has-ink" "only ${P_CONF_INKROWS} inked rows"
fi

# ════════════════════════════════════════════════════════════════════════
# 3. LINK. A program importing std.ocr AND NOTHING ELSE must build.
#
#    136.33 registered six modules' glue under the wrong name; that defect
#    is invisible unless the module is the sole import, because any other
#    import drags the glue in.  For std.ocr it would also catch a missing
#    -framework or -lobjc, which fails as undefined _objc_* symbols at
#    link -- the 136.3 keychain failure mode exactly.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/link.tk" <<'TKEOF'
m=ocrlink;
i=o:std.ocr;

f=main():i64{
  let r=o.runsfile("/nonexistent/none.png");
  mt r { $ok:runs runs.len; $err:e 0 };
  <0
};
TKEOF
if "${TKC}" --out "${WORK}/link" "${WORK}/link.tk" > "${WORK}/link.log" 2>&1; then
    expect "link-ocr-alone" "ok" "ok"
else
    fail_only "link-ocr-alone" "a program importing only std.ocr did not build"
    sed 's/^/        /' "${WORK}/link.log"
fi

# ════════════════════════════════════════════════════════════════════════
# 4. THE CONSUMER
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/ocr.tk" <<'TKEOF'
m=ocrconsumer;
i=io:std.io;
i=s:std.str;
i=f:std.fmt;
i=env:std.env;
i=fl:std.file;
i=o:std.ocr;

f=fix(name:str):str{
  <s.concat(s.concat(env.getor("OCRFIX";"/tmp/ocrfix");"/");name)
};

(* page|x|y|width|height|confidence|text -- EVERY declared field of $ocrrun.
   A .tki/glue slot mismatch still compiles and then reads whatever sits at
   the offset (127.86), and five of these seven slots carry an f64 BIT
   PATTERN, so every field is printed and compared.

   TEXT IS PRINTED LAST, which is NOT the .tki slot order, and the reason is
   a real one this suite hit: the recogniser returned `|||||1100` for the
   confusable fixture, so the recognised text CONTAINS the field delimiter.
   Text first meant the harness split a run into nine fields and compared
   the wrong ones -- reading "1" where it asked for a confidence of 1.0000.
   With text last every numeric field has a fixed index and the text is
   simply everything after the sixth delimiter, whatever it contains. *)
f=runline(r:$ocrrun):str{
  let v=mut.s.fromint(r.page);
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.x;2));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.y;2));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.width;2));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.height;2));
  v=s.concat(v;"|"); v=s.concat(v;f.f64(r.confidence;4));
  v=s.concat(v;"|"); v=s.concat(v;r.text);
  <v
};

f=each(tag:str;runs:@($ocrrun)):i64{
  io.println(s.concat(s.concat(tag;"-nruns=");s.fromint(runs.len)));
  lp(let i=0;i<runs.len;i=i+1){
    io.println(s.concat(s.concat(s.concat(s.concat(tag;"-r");s.fromint(i));"=");runline(runs.get(i))))
  };
  <0
};

f=dump(tag:str;name:str):i64{
  let rr=o.runsfile(fix(name));
  mt rr {
    $ok:runs each(tag;runs);
    $err:e io.println(s.concat(s.concat(tag;"-nruns=ERR|");o.lasterrkind()))
  };
  let t=o.textfile(fix(name));
  mt t {
    $ok:txt io.println(s.concat(s.concat(tag;"-text=");s.replace(txt;"\n";"/")));
    $err:e io.println(s.concat(s.concat(tag;"-text=ERR|");o.lasterrkind()))
  };
  <0
};

(* The in-memory path: ocr.runs over a @(byte), which must agree with
   ocr.runsfile run for run. *)
f=dumpmem(tag:str;name:str):i64{
  let b=fl.readbytes(fix(name));
  mt b {
    $ok:bytes memruns(tag;bytes);
    $err:e io.println(s.concat(tag;"-nruns=READERR"))
  };
  <0
};

f=memruns(tag:str;bytes:@(byte)):i64{
  let rr=o.runs(bytes);
  mt rr {
    $ok:runs each(tag;runs);
    $err:e io.println(s.concat(s.concat(tag;"-nruns=ERR|");o.lasterrkind()))
  };
  <0
};

(* The raw-pixel path: the composition point with std.image. *)
f=dumpraw(tag:str;name:str;w:i64;h:i64;ch:i64):i64{
  let b=fl.readbytes(fix(name));
  mt b {
    $ok:bytes rawruns(tag;bytes;w;h;ch);
    $err:e io.println(s.concat(tag;"-nruns=READERR"))
  };
  <0
};

f=rawruns(tag:str;bytes:@(byte);w:i64;h:i64;ch:i64):i64{
  let rr=o.runsraw(bytes;w;h;ch);
  mt rr {
    $ok:runs each(tag;runs);
    $err:e io.println(s.concat(s.concat(tag;"-nruns=ERR|");o.lasterrkind()))
  };
  <0
};

f=shortraw(tag:str;name:str):i64{
  let b=fl.readbytes(fix(name));
  mt b {
    $ok:bytes shortcall(tag;bytes);
    $err:e io.println(s.concat(tag;"-nruns=READERR"))
  };
  <0
};

f=shortcall(tag:str;bytes:@(byte)):i64{
  let rr=o.runsraw(bytes;900;300;1);
  mt rr {
    $ok:runs io.println(s.concat(s.concat(tag;"-nruns=");s.fromint(runs.len)));
    $err:e io.println(s.concat(tag;"-nruns=ERR"))
  };
  <0
};

f=main():i64{
  (* Availability, and the evidence that it is a PROBE and not a constant. *)
  io.println(s.concat("avail=";if(o.isavailable()){"yes"}el{"no"}));
  io.println(s.concat("engine=";o.engine()));
  io.println(s.concat("haslangs=";if(s.contains(o.languages();"en-US")){"yes"}el{"no"}));
  io.println(s.concat("langcount=";s.fromint((s.split(o.languages();",")).len)));
  io.println(s.concat("setlang-known=";if(o.setlanguages("en-US")){"yes"}el{"no"}));
  io.println(s.concat("setlang-unknown=";if(o.setlanguages("xx-YY")){"yes"}el{s.concat("no|";o.lasterrkind())}));
  (* A BCP-47 prefix of a supported tag is NOT a supported tag: "en" must be
     refused even though "en-US" is present, or a substring match is hiding
     in the implementation. *)
  io.println(s.concat("setlang-prefix=";if(o.setlanguages("en")){"yes"}el{s.concat("no|";o.lasterrkind())}));
  o.setlanguages("");

  dump("stmt";"statement.png");
  dumpmem("mem";"statement.png");
  dump("jpg";"statement.jpg");
  dumpraw("raw";"statement.raw";900;300;1);
  dump("blank";"blank.png");
  dump("tiny";"tiny.png");
  dump("notimg";"notanimage.png");
  dump("conf";"confusable.png");

  (* A path that does not exist is "io", distinct from "badimage". *)
  let miss=o.runsfile(fix("nosuchfile.png"));
  mt miss {
    $ok:runs io.println(s.concat("miss-nruns=";s.fromint(runs.len)));
    $err:e io.println(s.concat("miss-nruns=ERR|";o.lasterrkind()))
  };

  (* A buffer too small for the stated geometry must be refused, not read
     off the end of.  tiny.png is 12x12; claiming it is 900x300 greyscale
     asks the module to read 270000 bytes out of a few hundred. *)
  shortraw("short";"tiny.png");

  (* Fast level: a different recognition level on the SAME image. *)
  o.setfast(true);
  dump("fast";"statement.png");
  dump("fastconf";"confusable.png");
  o.setfast(false);
  dump("again";"statement.png");
  <0
};
TKEOF

if ! "${TKC}" --out "${WORK}/ocrrun" "${WORK}/ocr.tk" > "${WORK}/build.log" 2>&1; then
    fail_only "build-consumer" "the std.ocr consumer did not build"
    sed 's/^/        /' "${WORK}/build.log"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"
    exit 1
fi

OUT="$(OCRFIX="${FIX}" "${WORK}/ocrrun" --allow-read --allow-env 2>&1)"

if [ -z "${OUT}" ]; then
    fail_only "run-consumer" "the consumer printed nothing"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"
    exit 1
fi

# ════════════════════════════════════════════════════════════════════════
# 5. AVAILABILITY IS A PROBE, NOT A CONSTANT
#
# The recurring defect in this tree is an isavailable() that returns a
# hard-coded 0 or 1: std.mlx's tk_mlx_isavailable_w() does exactly that in
# BOTH arms of its #if, ignoring the real mlx_is_available() beside it.
#
# A constant cannot pass these four together.  The language list is read
# live out of the engine, so a compiled-in answer would have to hard-code
# thirty BCP-47 tags AND reject "xx-YY" AND reject "en" while accepting
# "en-US" -- at which point it is a probe.
# ════════════════════════════════════════════════════════════════════════
expect "avail-yes"            "$(line "${OUT}" avail)"           "yes"
expect "engine-is-named"      "$(line "${OUT}" engine)"          "vision"
expect "languages-live"       "$(line "${OUT}" haslangs)"        "yes"
expect "setlang-known-ok"     "$(line "${OUT}" setlang-known)"   "yes"
expect "setlang-unknown-kind" "$(line "${OUT}" setlang-unknown)" "no|nolanguage"
expect "setlang-prefix-kind"  "$(line "${OUT}" setlang-prefix)"  "no|nolanguage"
LANGCOUNT="$(line "${OUT}" langcount)"
if [ "${LANGCOUNT:-0}" -gt 5 ]; then
    echo "PASS languages-plural (${LANGCOUNT} tags, read live)"
    PASS=$((PASS + 1))
else
    fail_only "languages-plural" "only ${LANGCOUNT} tags"
fi

# ════════════════════════════════════════════════════════════════════════
# 6. EXACT TEXT, AND EXACT GEOMETRY AGAINST PILLOW'S OWN METRICS
#
# Not one of these is on non-emptiness.
# ════════════════════════════════════════════════════════════════════════
expect "stmt-nruns" "$(line "${OUT}" stmt-nruns)" "${P_NLINES}"

for i in 0 1 2; do
    R="$(line "${OUT}" "stmt-r${i}")"
    eval "WANT_TEXT=\${P_L${i}_TEXT}"
    eval "WANT_X=\${P_L${i}_X}"
    eval "WANT_W=\${P_L${i}_W}"
    eval "WANT_Y=\${P_L${i}_Y}"
    eval "WANT_H=\${P_L${i}_H}"

    expect "stmt-r${i}-text"  "$(runtext "${R}")"        "${WANT_TEXT}"
    expect "stmt-r${i}-page"  "$(field "${R}" ${F_PAGE})" "1"
    near   "stmt-r${i}-x"     "$(field "${R}" ${F_X})"    "${WANT_X}"
    near   "stmt-r${i}-y"     "$(field "${R}" ${F_Y})"    "${WANT_Y}"
    near   "stmt-r${i}-width" "$(field "${R}" ${F_W})"    "${WANT_W}"
    near   "stmt-r${i}-height" "$(field "${R}" ${F_H})"   "${WANT_H}"
    # Confidence is an f64 slot: a glue writing (int64_t)1.0 would print
    # 0.0000 here, which is the 127.86 slot-drift failure mode.
    expect "stmt-r${i}-confidence" "$(field "${R}" ${F_CONF})" "1.0000"
done

# THE ORIGIN.  Runs come back top line first, and with y INCREASING UPWARD
# that means each y is LOWER than the one before.  Get the origin wrong
# (raster convention, y down) and every string is still correct while the
# order is reversed -- which is exactly the bug a consumer doing OCR
# fallback would never notice until a statement's columns were misaligned.
Y0="$(field "$(line "${OUT}" stmt-r0)" ${F_Y})"
Y1="$(field "$(line "${OUT}" stmt-r1)" ${F_Y})"
Y2="$(field "$(line "${OUT}" stmt-r2)" ${F_Y})"
ORDER=$(awk -v a="${Y0}" -v b="${Y1}" -v c="${Y2}" \
            'BEGIN { print (a > b && b > c) ? "top-down-y-up" : "wrong" }')
expect "stmt-origin-bottom-left" "${ORDER}" "top-down-y-up"

# ocr.text joins the runs in the engine's order.
expect "stmt-text" "$(line "${OUT}" stmt-text)" "${P_EXPECT_TEXT}"

# ════════════════════════════════════════════════════════════════════════
# 7. THE SAME PICTURE THROUGH FOUR DOORS
#
# file / memory / a different codec / raw pixels must agree on the text.
# A disagreement means one path is decoding something else -- and the raw
# path is the one that composes with std.image, so a silent row-order or
# channel-order error there would corrupt every preprocessed page.
# ════════════════════════════════════════════════════════════════════════
expect "mem-nruns"  "$(line "${OUT}" mem-nruns)"  "${P_NLINES}"
expect "jpg-nruns"  "$(line "${OUT}" jpg-nruns)"  "${P_NLINES}"
expect "raw-nruns"  "$(line "${OUT}" raw-nruns)"  "${P_NLINES}"
for i in 0 1 2; do
    eval "WANT_TEXT=\${P_L${i}_TEXT}"
    expect "mem-r${i}-text" "$(runtext "$(line "${OUT}" "mem-r${i}")")" "${WANT_TEXT}"
    expect "jpg-r${i}-text" "$(runtext "$(line "${OUT}" "jpg-r${i}")")" "${WANT_TEXT}"
    expect "raw-r${i}-text" "$(runtext "$(line "${OUT}" "raw-r${i}")")" "${WANT_TEXT}"
done

# Determinism: the same image recognised twice in one process must give the
# same answer, or nothing asserted above means anything on a rerun.
expect "rerun-is-identical" "$(line "${OUT}" again-text)" "${P_EXPECT_TEXT}"

# ════════════════════════════════════════════════════════════════════════
# 8. EMPTY, UNAVAILABLE AND FAILED ARE FIVE DIFFERENT ANSWERS
#
# Requirement 5 of story 135.6. A module that collapses these tells a
# caller a scanned statement is empty -- the defect std.pdf refuses with a
# distinct $encrypted.
# ════════════════════════════════════════════════════════════════════════
# A blank page is a SUCCESS with zero runs, NOT an error.  The premise above
# proved the page really is blank.
expect "blank-is-success-not-error" "$(line "${OUT}" blank-nruns)" "0"
expect "blank-text-is-empty"        "$(line "${OUT}" blank-text)"  ""
# Each failure carries its OWN kind, by name.
expect "tiny-kind"   "$(line "${OUT}" tiny-nruns)"   "ERR|toosmall"
expect "notimg-kind" "$(line "${OUT}" notimg-nruns)" "ERR|badimage"
expect "miss-kind"   "$(line "${OUT}" miss-nruns)"   "ERR|io"
expect "short-raw-refused" "$(line "${OUT}" short-nruns)" "ERR"

# ════════════════════════════════════════════════════════════════════════
# 9. THE CASE THE RECOGNISER GETS WRONG — AND THE CONFIDENCE THAT DOES NOT
#    REFLECT IT
#
# This is the assertion the story asked for, and the answer it got is not
# the one the story assumed.  The image reads `Illlll1I0O`.  Vision returns
# `|||||1100`: the wrong characters, the wrong length, and not a plausible
# reading of the pixels by any standard.  Its confidence is EXACTLY 1.0 --
# the same value every correct line above scores.
#
# Both halves are asserted deliberately:
#   - the recognised text is NOT what the image says (so the fixture really
#     is a failure case and has not quietly started passing), and
#   - the confidence is 1.0 (so the absence of signal is pinned).
#
# If a future macOS puts real information in that number, BOTH of these
# change and this case fails loudly, which is the point of writing it down.
# ════════════════════════════════════════════════════════════════════════
CONF_RUN="$(line "${OUT}" conf-r0)"
expect "confusable-nruns"        "$(line "${OUT}" conf-nruns)" "1"
expect "confusable-is-wrong"     "$(runtext "${CONF_RUN}")"    "|||||1100"
expect "confusable-confidence-is-full" "$(field "${CONF_RUN}" ${F_CONF})" "1.0000"
if [ "$(runtext "${CONF_RUN}")" = "${CONFUSABLE}" ]; then
    fail_only "confusable-premise" \
      "the recogniser now reads this correctly; the wrong-answer fixture is stale"
else
    echo "PASS confusable-premise (returned '$(runtext "${CONF_RUN}")', image says '${CONFUSABLE}')"
    PASS=$((PASS + 1))
fi

# The one signal that IS real: at fast level the recogniser REFUSES this
# image rather than guessing at it.  Zero runs where accurate returned a
# confident wrong answer is information a caller can act on, and it is what
# docs/stdlib/ocr.md tells a caller to use instead of a confidence threshold.
expect "fastconf-refuses" "$(line "${OUT}" fastconf-nruns)" "0"
# ...and fast still reads the clean statement, so the refusal above is about
# the image and not about the level being broken.
expect "fast-still-reads-clean" "$(line "${OUT}" fast-text)" "${P_EXPECT_TEXT}"

# ════════════════════════════════════════════════════════════════════════
# 10. THE NO-ENGINE PATH, ACTUALLY BUILT AND ACTUALLY RUN
#
# `make build-all` cross-compiles SRCS. A stdlib module .c file is not in
# SRCS -- tkc compiles it at program link time, for the host only -- so the
# #else branch of every one of these modules is built by no gate at all.
# Compiling it here with -U__APPLE__ is the only thing in this repository
# that does, and it has already earned its place: it caught clear_err()
# being unused off Apple, which broke the build under -Werror.
#
# The assertion is requirement 5: NOENGINE, never an empty success.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/noengine.c" <<'CEOF'
#include "ocr.h"
#include <stdio.h>
int main(void)
{
    TkOcrPage *p;
    printf("avail=%d\n", ocr_is_available());
    printf("engine=%s\n", ocr_engine());
    printf("langs=[%s]\n", ocr_languages());
    p = ocr_recognize_mem((const unsigned char *)"x", 1);
    printf("mem=%s|%s\n", p ? "page" : "null", ocr_lasterr_kind());
    p = ocr_recognize_file("/etc/hosts");
    printf("file=%s|%s\n", p ? "page" : "null", ocr_lasterr_kind());
    p = ocr_recognize_raw((const unsigned char *)"x", 1, 1, 1);
    printf("raw=%s|%s\n", p ? "page" : "null", ocr_lasterr_kind());
    printf("setlang=%d|%s\n", ocr_set_languages("en-US"), ocr_lasterr_kind());
    printf("msglen=%d\n", (int)(ocr_lasterr()[0] != '\0'));
    return 0;
}
CEOF
if cc -std=c99 -Wall -Wextra -Wpedantic -Werror -U__APPLE__ \
      -I"${REPO_ROOT}/src/stdlib" -o "${WORK}/noengine" \
      "${WORK}/noengine.c" "${REPO_ROOT}/src/stdlib/ocr.c" \
      > "${WORK}/noengine.log" 2>&1; then
    echo "PASS noengine-branch-compiles"
    PASS=$((PASS + 1))
    NOUT="$("${WORK}/noengine" 2>&1)"
    expect "noengine-unavailable" "$(line "${NOUT}" avail)"   "0"
    expect "noengine-engine-name" "$(line "${NOUT}" engine)"  "none"
    expect "noengine-no-langs"    "$(line "${NOUT}" langs)"   "[]"
    # Every entry point FAILS. None returns an empty success.
    expect "noengine-mem"     "$(line "${NOUT}" mem)"     "null|noengine"
    expect "noengine-file"    "$(line "${NOUT}" file)"    "null|noengine"
    expect "noengine-raw"     "$(line "${NOUT}" raw)"     "null|noengine"
    expect "noengine-setlang" "$(line "${NOUT}" setlang)" "0|noengine"
    expect "noengine-has-message" "$(line "${NOUT}" msglen)" "1"
else
    fail_only "noengine-branch-compiles" \
      "src/stdlib/ocr.c does not compile with -U__APPLE__; std.ocr is broken off macOS"
    sed 's/^/        /' "${WORK}/noengine.log"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
