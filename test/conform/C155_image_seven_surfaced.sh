#!/usr/bin/env bash
# C155_image_seven_surfaced.sh — the seven finished-but-unreachable std.image
# transforms are reachable FROM TOKE and compute the right pixels (135.5c).
#
# image_sharpen, image_brightness, image_contrast, image_paste,
# image_histogram, image_quantize and image_text_draw were complete in
# src/stdlib/image.c (34.3.1 / 34.3.2, hardened by 122.9) with no wrapper in
# image_glue.c and no entry in stdlib/image.tki, so a toke call to any of them
# did not compile.  Same class as 135.5's rotate and blur (C030).
#
# As in C030, every assertion is on bytes: the toke program writes each
# result's raw pixel plane with file.writebytes(img.data), and python3
# recomputes the expected plane itself from the documented formulas, with no
# reference to image.c.  A transform that returned its input would pass any
# "right size" check, so each fixture is chosen so that the identity fails.
#
# Story: 135.5c

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
GLUE="${REPO_ROOT}/src/stdlib/image_glue.c"
TKI="${REPO_ROOT}/stdlib/image.tki"

PASS=0
FAIL=0

ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
finish() {
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"
    [ "${FAIL}" -eq 0 ]
    exit $?
}

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c155_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "C155: std.image sharpen/brightness/contrast/paste/histogram/quantize/textdraw from toke"
echo "--------------------------------------"

# ── Part 1: glue and interface ───────────────────────────────────────────────
for sym in tk_image_sharpen_w tk_image_brightness_w tk_image_contrast_w \
           tk_image_paste_w tk_image_histogram_w tk_image_quantize_w \
           tk_image_textdraw_w; do
    if grep -qE "^int64_t ${sym}\(" "${GLUE}"; then
        ok "${sym} is defined in image_glue.c"
    else
        bad "${sym} is not defined in image_glue.c"
    fi
done

if python3 - "${TKI}" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
want = {
    "image.sharpen":    (["imgbuf"], "imgbuf"),
    "image.brightness": (["imgbuf", "f64"], "imgbuf"),
    "image.contrast":   (["imgbuf", "f64"], "imgbuf"),
    "image.paste":      (["imgbuf", "imgbuf", "i64", "i64"], "imgbuf"),
    "image.histogram":  (["imgbuf"], "[i64]"),
    "image.quantize":   (["imgbuf", "u32"], "imgbuf"),
    "image.textdraw":   (["imgbuf", "str", "i64", "i64", "u32", "u32"], "imgbuf"),
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
PY
then
    ok "stdlib/image.tki declares all seven with real types (not the i64 ABI placeholder)"
else
    bad "stdlib/image.tki does not declare all seven with the expected types"
fi

# ── Part 2: fixtures ─────────────────────────────────────────────────────────
cd "${WORK}"
python3 - "${WORK}" <<'PY'
import os, sys
W = sys.argv[1]
# 5x5 grey ramp with a bright centre: sharpening must change it.
g = bytearray((x * 20 + y * 10) & 0xFF for y in range(5) for x in range(5))
g[12] = 200
open(os.path.join(W, 'g.gray'), 'wb').write(bytes(g))
# 3x2 RGBA: values span 0..255, alpha varies (brightness/contrast keep alpha).
rgba = bytes([10, 100, 200, 7,   128, 0, 255, 99,   60, 90, 120, 255,
              255, 1, 64, 0,     33, 66, 99, 128,   200, 150, 100, 50])
open(os.path.join(W, 'rgba.raw'), 'wb').write(rgba)
# 4x4 grey dst (all 50), 2x2 grey src (1..4) for paste.
open(os.path.join(W, 'dst.gray'), 'wb').write(bytes([50] * 16))
open(os.path.join(W, 'src.gray'), 'wb').write(bytes([1, 2, 3, 4]))
# 4x1 RGB with two clearly separated colour clusters, for quantize to 2.
open(os.path.join(W, 'q.rgb'), 'wb').write(bytes([10, 10, 10, 20, 20, 20,
                                                  200, 200, 200, 220, 220, 220]))
# 12x9 grey canvas of zeros for text.
open(os.path.join(W, 'canvas.gray'), 'wb').write(bytes(12 * 9))
PY
[ -s g.gray ] && [ -s q.rgb ] || { bad "fixtures were not produced"; finish; }

cat > c155.tk <<EOF
m=c155;
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
  let wrote = mt file.writebytes(str.concat("${WORK}/"; str.concat(name; ".out")); img.data) {\$ok:v v;\$err:e false};
  <0
};

f=rd(name:str):@(i64){
  <mt file.readbytes(str.concat("${WORK}/"; name)) {\$ok:d d;\$err:e @()}
};

f=main():i64{
  let g    = image.fromraw(rd("g.gray"); 5; 5; 1);
  let rgba = image.fromraw(rd("rgba.raw"); 3; 2; 4);
  let dst  = image.fromraw(rd("dst.gray"); 4; 4; 1);
  let src  = image.fromraw(rd("src.gray"); 2; 2; 1);
  let q    = image.fromraw(rd("q.rgb"); 4; 1; 3);
  let cv   = image.fromraw(rd("canvas.gray"); 12; 9; 1);

  dump(image.sharpen(g); "sharpen");
  dump(image.brightness(rgba; 1.5); "bright");
  dump(image.contrast(rgba; 2.0); "contrast");
  dump(image.paste(dst; src; 1; 2); "paste");
  dump(image.paste(dst; src; 3; 3); "pasteclip");
  (* an offset past 2^32 must stay off the image, not wrap back to 0 *)
  dump(image.paste(dst; src; 4294967296; 0); "pastefar");
  dump(image.quantize(q; 2); "quant");
  dump(image.textdraw(cv; "I"; 1; 1; 1; 4278190080); "text");

  let h = image.histogram(rgba);
  say("hist.len="; h.len);
  say("hist.r10="; h.get(10));
  say("hist.r255="; h.get(255));
  say("hist.g0="; h.get(256));
  say("hist.b255="; h.get(512 + 255));
  say("hist.a255="; h.get(768 + 255));
  let hg = image.histogram(g);
  say("histg.r200="; hg.get(200));
  say("histg.g200="; hg.get(456));

  (* refused input: quantize needs 3+ channels; an empty result, not a crash *)
  let qg = image.quantize(g; 4);
  say("quantgray.w="; qg.width as i64);
  <0
};
EOF

if "${TKC}" --allow-all -o c155.bin c155.tk >build.log 2>&1; then
    ok "a toke program compiles calls to all seven entry points"
else
    bad "c155.tk did not compile — the entry points are not reachable from toke"
    sed 's/^/      /' build.log | head -12
    finish
fi

if ./c155.bin > run.log 2>&1; then
    ok "the toke program ran to completion"
else
    bad "the toke program exited non-zero"
    sed 's/^/      /' run.log | head -12
fi
if [ -s run.log ] && grep -q '^hist.len=' run.log; then
    ok "the toke program produced its report"
else
    bad "the toke program produced no usable output: [$(head -3 run.log 2>/dev/null)]"
    finish
fi

# ── Part 3: pixels, recomputed independently ─────────────────────────────────
python3 - "${WORK}" > verdicts.txt <<'PY'
import os, sys
W = sys.argv[1]
log = {}
for line in open(os.path.join(W, 'run.log')):
    if '=' in line:
        k, _, v = line.strip().partition('=')
        log[k] = v
def rd(n):
    p = os.path.join(W, n)
    return open(p, 'rb').read() if os.path.exists(p) else None
def say(c, m): print(("OK " if c else "NO ") + m)
def clamp(v):
    v = int(v + 0.5) if v >= 0 else 0   # clamp_u8 rounds half up
    return max(0, min(255, v))

# sharpen: [0,-1,0;-1,5,-1;0,-1,0], clamp-to-edge
g = rd('g.gray'); W5 = 5
want = []
for y in range(5):
    for x in range(5):
        def px(xx, yy): return g[min(4, max(0, yy)) * W5 + min(4, max(0, xx))]
        acc = 5 * px(x, y) - px(x-1, y) - px(x+1, y) - px(x, y-1) - px(x, y+1)
        want.append(max(0, min(255, acc)))
got = rd('sharpen.out')
say(got == bytes(want), "sharpen: all 25 pixels match the 5/-1 kernel (got %s)" % (list(got) if got else got))
say(got != g, "sharpen: the output is not the input")

rgba = rd('rgba.raw')
def per_px(f):
    out = bytearray(rgba)
    for i in range(0, len(out), 4):
        for c in range(3): out[i+c] = f(rgba[i+c])
    return bytes(out)
say(rd('bright.out') == per_px(lambda v: clamp(v * 1.5)),
    "brightness 1.5: colour channels scaled and clamped, alpha untouched (got %s)" % list(rd('bright.out') or b''))
say(rd('contrast.out') == per_px(lambda v: clamp(128 + (v - 128) * 2.0)),
    "contrast 2.0: 128 + (v-128)*2 clamped, alpha untouched (got %s)" % list(rd('contrast.out') or b''))

def paste(x0, y0):
    d = bytearray([50] * 16); s = [1, 2, 3, 4]
    for sy in range(2):
        for sx in range(2):
            dx, dy = x0 + sx, y0 + sy
            if 0 <= dx < 4 and 0 <= dy < 4: d[dy * 4 + dx] = s[sy * 2 + sx]
    return bytes(d)
say(rd('paste.out') == paste(1, 2), "paste at (1,2): src lands there and nowhere else")
say(rd('pasteclip.out') == paste(3, 3), "paste at (3,3): clipped to the one pixel inside dst")
say(rd('pastefar.out') == bytes([50] * 16),
    "paste at x=2^32: nothing pasted (an int truncation would have pasted at x=0)")

q = rd('quant.out')
say(q == bytes([15, 15, 15, 15, 15, 15, 210, 210, 210, 210, 210, 210]),
    "quantize to 2: each cluster maps to its mean colour (got %s)" % (list(q) if q else q))
say(log.get('quantgray.w') == '0', "quantize of a 1-channel image is refused (empty imgbuf), not a crash")

# textdraw: 'I' glyph rows 0E 04 04 04 04 04 0E, bit 0x10 = column 0, at (1,1)
I = [0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E]
want_t = bytearray(12 * 9)
for r, bits in enumerate(I):
    for c in range(5):
        if bits & (0x10 >> c): want_t[(1 + r) * 12 + 1 + c] = 255
t = rd('text.out')
say(t == bytes(want_t), "textdraw 'I' at (1,1) size 1: exactly the 5x7 glyph's pixels, grey takes the R byte")

say(log.get('hist.len') == '1024', "histogram returns 1024 counts (r, g, b, a x 256)")
say((log.get('hist.r10'), log.get('hist.r255'), log.get('hist.g0'), log.get('hist.b255'), log.get('hist.a255'))
    == ('1', '1', '1', '1', '1'), "histogram RGBA: r[10], r[255], g[0], b[255], a[255] are each 1")
say(log.get('histg.r200') == '1' and log.get('histg.g200') == '0',
    "histogram grey: counts go to r only")
PY

while IFS= read -r line; do
    case "${line}" in
        "OK "*) ok "${line#OK }" ;;
        "NO "*) bad "${line#NO }" ;;
    esac
done < verdicts.txt
[ -s verdicts.txt ] || bad "the pixel checks produced no verdicts"

finish
