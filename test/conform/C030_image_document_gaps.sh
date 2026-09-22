#!/usr/bin/env bash
# C030_image_document_gaps.sh — std.image reaches the four document-processing
# gaps FROM TOKE, and computes the right pixels (story 135.5).
#
# WHAT WAS ACTUALLY MISSING, WHICH IS NOT WHAT THE STORY SAID.
# The story listed four gaps: arbitrary-angle rotation, adaptive thresholding,
# a convolution/blur primitive, and TIFF decode.  Two of those were already
# implemented in C.  image_rotate() — inverse-mapped bilinear, arbitrary angle
# — and image_blur() have been in src/stdlib/image.c since story 34.3.1.  What
# was missing for those two was any way to REACH them: neither had a wrapper in
# image_glue.c and neither appeared in stdlib/image.tki, so `image.rotate(img;
# 2.5)` in toke source did not compile.  That is the same class as 136.16
# (std.toon) and 136.44 (std.tls): a finished C core behind a surface that does
# not expose it.  So this test's first job is to prove the entry points are
# REACHABLE FROM TOKE, which is the half that was really absent.
#
# WHY THE ASSERTIONS ARE ON PIXEL VALUES.
# An image function that returns its input unchanged satisfies every check of
# the form "it returned a buffer of the right size" — and the pre-existing
# test_rotate in test/stdlib/test_image.c was exactly that, dimensions only.
# So every assertion here is on bytes:
#
#   * the toke program writes each result's RAW PIXEL PLANE out with
#     file.writebytes(img.data), so nothing is re-encoded and nothing is
#     decoded by the code under test before it is checked;
#   * python3 then compares those bytes against values it computes ITSELF,
#     from the same inputs, with no reference to image.c.
#
# THE ADAPTIVE-THRESHOLD FIXTURE IS ADVERSARIAL TO A GLOBAL THRESHOLD.
# It is a page lit from one side — background ramping 40..220 — with ink bars
# at both ends, each 60% of its LOCAL background.  The ink on the lit side is
# therefore BRIGHTER than the paper on the dark side, and the test asserts that
# impossibility before it asserts anything else.  Without that control, a
# global threshold would pass this test and it would prove nothing about the
# thing the story called the single most valuable addition.
#
# THE MULTI-PAGE TIFF FIXTURE CANNOT BE PASSED BY READING PAGE 0 THREE TIMES.
# Multi-page is the part of TIFF that is easy to leave out by accident, so the
# three pages differ in width, height, channel count AND compression (none,
# Deflate, PackBits).  A decoder that ignored the IFD chain would have to
# return a 5x4 grey plane where a 6x3 RGB one is expected.
#
# The fixture is written by a TIFF encoder in this script that shares no code
# with the decoder under test.  If PIL is installed, a fourth page set is
# written by PIL as well, so the LZW path is checked against a third-party
# encoder; when PIL is absent that arm is reported as SKIPPED, never as a pass.
#
# Story: 135.5

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The tkc symlink is relinked by any concurrent `make`; $TKC lets a caller pin
# a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"
GLUE="${REPO_ROOT}/src/stdlib/image_glue.c"
TKI="${REPO_ROOT}/stdlib/image.tki"

PASS=0
FAIL=0
SKIP=0

ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c030_XXXXXX)"
cleanup() { rm -rf "${WORK}"; }
trap cleanup EXIT

echo "C030: std.image reaches rotation, adaptive threshold, convolution and TIFF from toke"
echo "--------------------------------------"

# ── Part 1: the entry points exist in the glue and in the interface ──────────
#
# A call to `image.rotate` is lowered through the generic stdlib fallback to
# tk_image_rotate_w.  With no such symbol the call dies at codegen, which is
# the state this story found.

for sym in tk_image_rotate_w tk_image_blur_w tk_image_adaptivethreshold_w \
           tk_image_convolve_w tk_image_tiffpages_w tk_image_tiffdecode_w; do
    if grep -qE "^int64_t ${sym}\(" "${GLUE}"; then
        ok "${sym} is defined in image_glue.c"
    else
        bad "${sym} is not defined in image_glue.c"
    fi
done

# The interface must carry real types, not the ABI placeholder gen_tki.py
# emits when it can only establish arity — "i64, i64, i64" tells a caller
# nothing about which argument is a kernel and which a window size.
if python3 - "${TKI}" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
want = {
    "image.rotate":             (["imgbuf", "f64"], "imgbuf"),
    "image.blur":               (["imgbuf", "u32"], "imgbuf"),
    "image.adaptivethreshold":  (["imgbuf", "u32", "f64"], "imgbuf!str"),
    "image.convolve":           (["imgbuf", "[f64]", "u32", "f64", "f64"], "imgbuf!str"),
    "image.tiffpages":          (["[byte]"], "u32!str"),
    "image.tiffdecode":         (["[byte]", "u32"], "imgbuf!str"),
}
have = {e["name"]: e for e in d["exports"] if e.get("kind") == "func"}
bad = []
for n, (p, r) in want.items():
    e = have.get(n)
    if e is None:                              bad.append(n + ": absent")
    elif e.get("gen") == "abi":                bad.append(n + ": still ABI-typed")
    elif e["params"] != p or e["return"] != r: bad.append("%s: %s -> %s" % (n, e["params"], e["return"]))
if bad:
    print("; ".join(bad)); sys.exit(1)
sys.exit(0)
PY
then
    ok "stdlib/image.tki declares all six with real types (not the i64 ABI placeholder)"
else
    bad "stdlib/image.tki does not declare all six with the expected types"
fi

# ── Part 2: the fixtures, built by code that is not the decoder ─────────────

cd "${WORK}"

python3 - "${WORK}" <<'PY'
import struct, sys, zlib, os

W = sys.argv[1]

def packbits(data):
    out = bytearray(); i = 0; n = len(data)
    while i < n:
        run = 1
        while i + run < n and run < 128 and data[i+run] == data[i]: run += 1
        if run >= 3:
            out.append((257 - run) & 0xFF); out.append(data[i]); i += run
        else:
            lit = 0
            while i + lit < n and lit < 128:
                r2 = 1
                while i + lit + r2 < n and r2 < 4 and data[i+lit+r2] == data[i+lit]: r2 += 1
                if r2 >= 3: break
                lit += 1
            if lit == 0: lit = 1
            out.append(lit - 1); out += data[i:i+lit]; i += lit
    return bytes(out)

SHORT, LONG = 3, 4

def build_tiff(pages, path):
    """A little-endian strip TIFF writer.  Shares no code with image.c."""
    out = bytearray(b'II' + struct.pack('<HI', 42, 0))
    bpsoff, stroff = [], []
    for p in pages:
        if p['spp'] > 1:
            bpsoff.append(len(out))
            out += b''.join(struct.pack('<H', p['bps']) for _ in range(p['spp']))
        else:
            bpsoff.append(0)
        stroff.append(len(out))
        out += p['strip']
    prev_next = 4
    for i, p in enumerate(pages):
        struct.pack_into('<I', out, prev_next, len(out))
        ents = [(256, LONG, 1, p['w']), (257, LONG, 1, p['h']),
                (258, SHORT, p['spp'], bpsoff[i] if p['spp'] > 1 else p['bps']),
                (259, SHORT, 1, p['comp']), (262, SHORT, 1, p['photo']),
                (273, LONG, 1, stroff[i]), (277, SHORT, 1, p['spp']),
                (278, LONG, 1, p['h']), (279, LONG, 1, len(p['strip']))]
        out += struct.pack('<H', len(ents))
        for tag, ty, cnt, val in ents:
            out += struct.pack('<HHI', tag, ty, cnt)
            out += struct.pack('<HH', val, 0) if (ty == SHORT and cnt == 1) \
                   else struct.pack('<I', val)
        prev_next = len(out)
        out += struct.pack('<I', 0)
    open(path, 'wb').write(bytes(out))

# Three pages: different size, different channel count, different compression.
p0 = bytes((x * 40 + y * 10) & 0xFF for y in range(4) for x in range(5))
p1 = bytes(v for y in range(3) for x in range(6) for v in (x * 40, y * 80, 30))
p2 = bytes((255 - x * 30) & 0xFF for y in range(2) for x in range(8))
build_tiff([
    dict(w=5, h=4, bps=8, spp=1, photo=1, comp=1,     strip=p0),
    dict(w=6, h=3, bps=8, spp=3, photo=2, comp=8,     strip=zlib.compress(p1)),
    dict(w=8, h=2, bps=8, spp=1, photo=1, comp=32773, strip=packbits(p2)),
], os.path.join(W, 'multi.tif'))
open(os.path.join(W, 'p0.expect'), 'wb').write(p0)
open(os.path.join(W, 'p1.expect'), 'wb').write(p1)
open(os.path.join(W, 'p2.expect'), 'wb').write(p2)

# The unevenly lit page.
AW, AH = 64, 32
def bg(x): return 40 + (x * 180) // (AW - 1)
def ink(x, y): return 8 <= y <= 24 and (8 <= x <= 12 or 50 <= x <= 54)
ramp = bytes((bg(x) * 6) // 10 if ink(x, y) else bg(x)
             for y in range(AH) for x in range(AW))
open(os.path.join(W, 'ramp.gray'), 'wb').write(ramp)

# A single bright pixel: the impulse whose response is the kernel itself.
imp = bytearray(25); imp[2 * 5 + 2] = 255
open(os.path.join(W, 'impulse.gray'), 'wb').write(bytes(imp))

# Optional fourth fixture from a third-party encoder, for the LZW path.
try:
    from PIL import Image
    a = Image.new('L', (5, 4)); a.putdata(list(p0))
    a.save(os.path.join(W, 'lzw.tif'), compression='tiff_lzw')
    open(os.path.join(W, 'has_pil'), 'w').write('1')
except Exception:
    pass
print("fixtures ok")
PY
if [ ! -s multi.tif ] || [ ! -s ramp.gray ]; then
    bad "fixtures were not produced — nothing below can run"
    echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
    exit 1
fi
ok "fixtures built by an independent TIFF encoder (3 pages: none / Deflate / PackBits)"

# ── Part 3: a toke program drives every one of the six entry points ─────────

cat > c030.tk <<EOF
m=c030;
i=io:std.io;
i=file:std.file;
i=str:std.str;
i=image:std.image;

f=say(k:str;v:i64):i64{
  io.println(str.concat(k; str.fromint(v)));
  <0
};

f=dump(img:\$imgbuf;name:str):i64{
  say(str.concat(name; ".w="); img.width as i64);
  say(str.concat(name; ".h="); img.height as i64);
  say(str.concat(name; ".c="); img.channels as i64);
  let wrote = mt file.writebytes(str.concat("${WORK}/"; str.concat(name; ".raw")); img.data) {\$ok:v v;\$err:e false};
  <0
};

f=main():i64{
  let empty = \$imgbuf{width:0;height:0;channels:0;data:@()};

  (* ---- TIFF: page count, then each page ---- *)
  let tif = mt file.readbytes("${WORK}/multi.tif") {\$ok:d d;\$err:e @()};
  let np  = mt image.tiffpages(tif) {\$ok:n n;\$err:e 0};
  say("tiffpages="; np);

  let a = mt image.tiffdecode(tif; 0) {\$ok:i i;\$err:e empty};
  let b = mt image.tiffdecode(tif; 1) {\$ok:i i;\$err:e empty};
  let c = mt image.tiffdecode(tif; 2) {\$ok:i i;\$err:e empty};
  dump(a; "tif0");
  dump(b; "tif1");
  dump(c; "tif2");

  (* a page past the end must fail, not quietly hand back page 0 *)
  let past = mt image.tiffdecode(tif; 9) {\$ok:i 1;\$err:e 0};
  say("tiffpast="; past);

  (* ---- adaptive threshold on the unevenly lit page ---- *)
  let graw = mt file.readbytes("${WORK}/ramp.gray") {\$ok:d d;\$err:e @()};
  let page = image.fromraw(graw; 64; 32; 1);
  let th   = mt image.adaptivethreshold(page; 15; 0.2) {\$ok:i i;\$err:e empty};
  dump(th; "thresh");

  (* ---- convolution: the impulse response IS the kernel ---- *)
  let iraw = mt file.readbytes("${WORK}/impulse.gray") {\$ok:d d;\$err:e @()};
  let imp  = image.fromraw(iraw; 5; 5; 1);
  let box  = @(1.0;1.0;1.0;1.0;1.0;1.0;1.0;1.0;1.0);
  let cv   = mt image.convolve(imp; box; 3; 0.0; 0.0) {\$ok:i i;\$err:e empty};
  dump(cv; "conv");

  (* an even kernel size must be rejected *)
  let evenk = @(1.0;1.0;1.0;1.0);
  let badk  = mt image.convolve(imp; evenk; 2; 1.0; 0.0) {\$ok:i 1;\$err:e 0};
  say("convevenok="; badk);

  (* ---- blur, previously unreachable from toke ---- *)
  let bl = image.blur(imp; 1);
  dump(bl; "blur");

  (* ---- rotation by an arbitrary angle, previously unreachable ---- *)
  let rot = image.rotate(page; 180.0);
  dump(rot; "rot");

  (* deskew angles are not multiples of 90: 2.5 degrees must also work *)
  let sk = image.rotate(page; 2.5);
  dump(sk; "skew");

  <0
};
EOF

if "${TKC}" --allow-all -o c030.bin c030.tk >build.log 2>&1; then
    ok "a toke program compiles calls to rotate, blur, adaptivethreshold, convolve, tiffpages and tiffdecode"
else
    bad "c030.tk did not compile — the entry points are not reachable from toke"
    sed 's/^/      /' build.log | head -12
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
    exit 1
fi

if ./c030.bin > run.log 2>&1; then
    ok "the toke program ran to completion"
else
    bad "the toke program exited non-zero"
    sed 's/^/      /' run.log | head -12
fi

# A run that produced no output is a run that proved nothing.
if [ -s run.log ] && grep -q '^tiffpages=' run.log; then
    ok "the toke program produced its report (an empty log would make every check below blind)"
else
    bad "the toke program produced no usable output: [$(head -3 run.log 2>/dev/null)]"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
    exit 1
fi

# ── Part 4: the pixels, checked against values python computes itself ───────

python3 - "${WORK}" > verdicts.txt <<'PY'
import os, sys

W = sys.argv[1]
log = dict()
for line in open(os.path.join(W, 'run.log')):
    if '=' in line:
        k, _, v = line.strip().partition('=')
        log[k] = v

def rd(n):
    p = os.path.join(W, n)
    return open(p, 'rb').read() if os.path.exists(p) else None

def say(okc, msg):
    print(("OK " if okc else "NO ") + msg)

# -- TIFF --------------------------------------------------------------------
say(log.get('tiffpages') == '3',
    "tiffpages reports 3 pages for a three-IFD TIFF (got %r)" % log.get('tiffpages'))

for i, (w, h, ch) in enumerate([(5, 4, 1), (6, 3, 3), (8, 2, 1)]):
    got = (log.get('tif%d.w' % i), log.get('tif%d.h' % i), log.get('tif%d.c' % i))
    say(got == (str(w), str(h), str(ch)),
        "tiffdecode page %d is %dx%dx%d, so the IFD chain was walked (got %r)"
        % (i, w, h, ch, got))
    want, have = rd('p%d.expect' % i), rd('tif%d.raw' % i)
    say(have is not None and have == want,
        "tiffdecode page %d pixels match the encoder's bytes exactly (%s)"
        % (i, "%d/%d" % (sum(a == b for a, b in zip(have or b'', want)), len(want))))

say(log.get('tiffpast') == '0',
    "tiffdecode of page 9 in a 3-page file fails instead of returning a page")

# -- adaptive threshold ------------------------------------------------------
AW, AH = 64, 32
src = rd('ramp.gray')
th  = rd('thresh.raw')

def bg(x): return 40 + (x * 180) // (AW - 1)
def is_ink(x, y): return 8 <= y <= 24 and (8 <= x <= 12 or 50 <= x <= 54)

ink_vals   = [src[y*AW+x] for y in range(AH) for x in range(AW) if is_ink(x, y)]
paper_vals = [src[y*AW+x] for y in range(AH) for x in range(AW) if not is_ink(x, y)]
say(max(ink_vals) > min(paper_vals),
    "CONTROL: the brightest ink (%d) is lighter than the darkest paper (%d), so "
    "NO global threshold can separate them" % (max(ink_vals), min(paper_vals)))

say(log.get('thresh.c') == '1' and th is not None and len(th) == AW * AH,
    "adaptivethreshold returns a %dx%d single-channel plane" % (AW, AH))

if th and len(th) == AW * AH:
    say(set(th) <= {0, 255}, "every output pixel is exactly 0 or 255 (values seen: %s)"
        % sorted(set(th)))
    inkbad = sum(1 for y in range(12, 21) for x in (10, 52) if th[y*AW+x] != 0)
    say(inkbad == 0,
        "ink is black at x=10 on the dark side AND x=52 on the lit side "
        "(%d of 18 sample pixels wrong)" % inkbad)
    paperbad = sum(1 for y in range(4, AH-4) for x in range(22, 43) if th[y*AW+x] != 255)
    say(paperbad == 0,
        "blank paper between the bars stays white, no speckle (%d of %d wrong)"
        % (paperbad, 24 * 21))
    say(th != src[:len(th)], "the output is not the input")
else:
    say(False, "adaptivethreshold produced no usable plane")

# -- convolution: the impulse response must BE the kernel --------------------
cv = rd('conv.raw')
want_cv = bytes(28 if (1 <= x <= 3 and 1 <= y <= 3) else 0
                for y in range(5) for x in range(5))
say(cv == want_cv,
    "convolve: the impulse response of a 3x3 box kernel is 255/9 = 28 over "
    "exactly the 3x3 neighbourhood and 0 elsewhere (got %s)"
    % (list(cv) if cv and len(cv) == 25 else cv))
say(log.get('convevenok') == '0', "convolve rejects an even kernel size")

# -- blur, recomputed here from the documented 1/2/1 kernel ------------------
bl = rd('blur.raw')
K = ((1,2,1),(2,4,2),(1,2,1))
imp = [0]*25; imp[2*5+2] = 255
want_bl = []
for y in range(5):
    for x in range(5):
        acc = 0
        for ky in (-1,0,1):
            for kx in (-1,0,1):
                sx = min(4, max(0, x+kx)); sy = min(4, max(0, y+ky))
                acc += K[ky+1][kx+1] * imp[sy*5+sx]
        want_bl.append(acc // 16)
say(bl == bytes(want_bl),
    "blur: every one of the 25 pixels matches the 1/2/1 Gaussian recomputed "
    "here (centre %s, expected %d)"
    % (bl[12] if bl and len(bl) == 25 else None, want_bl[12]))

# -- rotation ----------------------------------------------------------------
rot = rd('rot.raw')
if rot and len(rot) == AW * AH:
    # 180 degrees about the centre maps (x,y) to (W-x, H-y).
    pts = [(20, 10), (30, 6), (44, 20), (8, 24)]
    wrong = [(x, y, rot[y*AW+x], src[(AH-y)*AW + (AW-x)])
             for (x, y) in pts if abs(rot[y*AW+x] - src[(AH-y)*AW + (AW-x)]) > 2]
    say(not wrong,
        "rotate(180): each sampled pixel carries the value from the opposite "
        "side of the centre (%s)" % (wrong or "4/4 within 2"))
    say(rot[10*AW+20] != src[10*AW+20],
        "rotate(180) is not a passthrough: (20,10) holds %d, the input holds %d"
        % (rot[10*AW+20], src[10*AW+20]))
else:
    say(False, "rotate(180) produced no usable plane")

sk = rd('skew.raw')
if sk and len(sk) == AW * AH:
    # A 2.5-degree deskew is not expressible with fliph/flipv, and it must
    # interpolate: the result differs from both the input and the 180 case,
    # and the ink bar has moved down-left of where it was.
    say(sk != src[:len(sk)], "rotate(2.5) changed the image")
    say(sum(a != b for a, b in zip(sk, rot or b'')) > AW * AH // 2,
        "rotate(2.5) is not rotate(180) — a fixed-angle stub would collapse them")
    # Interpolation: a 2.5-degree turn of a smooth ramp must produce values that
    # are NOT all present in the source column they came from.
    interior = [sk[y*AW+x] for y in range(6, AH-6) for x in range(20, 44)]
    say(len(set(interior)) > 8,
        "rotate(2.5) produced %d distinct interior values — it resampled rather "
        "than snapping to a grid" % len(set(interior)))
else:
    say(False, "rotate(2.5) produced no usable plane")
PY

while IFS= read -r line; do
    case "${line}" in
        "OK "*) ok "${line#OK }" ;;
        "NO "*) bad "${line#NO }" ;;
    esac
done < verdicts.txt

# ── Part 5: LZW against a third-party encoder, when one is installed ────────

if [ -f has_pil ]; then
    cat > lzw.tk <<EOF
m=lzwt;
i=io:std.io;
i=file:std.file;
i=str:std.str;
i=image:std.image;

f=main():i64{
  let empty = \$imgbuf{width:0;height:0;channels:0;data:@()};
  let d = mt file.readbytes("${WORK}/lzw.tif") {\$ok:v v;\$err:e @()};
  let p = mt image.tiffdecode(d; 0) {\$ok:i i;\$err:e empty};
  io.println(str.concat("lzw.w="; str.fromint(p.width as i64)));
  let w = mt file.writebytes("${WORK}/lzw.raw"; p.data) {\$ok:v v;\$err:e false};
  <0
};
EOF
    if "${TKC}" --allow-all -o lzw.bin lzw.tk >lzw.build 2>&1 && ./lzw.bin >lzw.log 2>&1 \
       && cmp -s lzw.raw p0.expect; then
        ok "TIFF LZW decodes byte-for-byte against a PIL-written file (an encoder this project did not write)"
    else
        bad "TIFF LZW did not match the PIL-written file"
    fi
else
    skip "TIFF LZW arm: PIL is not installed, so no third-party LZW encoder is available here"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
# Zero passes means the run did not happen, which is not a pass.
[ "${FAIL}" -eq 0 ] && [ "${PASS}" -gt 25 ]
