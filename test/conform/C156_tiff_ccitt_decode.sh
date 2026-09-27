#!/usr/bin/env bash
# C156_tiff_ccitt_decode.sh — image.tiffdecode reads CCITT G3/G4 bilevel
# pages, the compression scanners and fax gateways emit (story 135.5a).
#
# Before 135.5a compression 2 (Modified Huffman), 3 (T.4 / Group 3) and 4
# (T.6 / Group 4) were refused with "CCITT G3/G4 fax compression is not
# supported".  This suite checks the new decoder against an encoder that
# shares no code with it: libtiff.
#
# THE FIXTURES.  test/fuzz/seeds/image/ccitt_{g4,g3_2d,mh}.tif were written by
# libtiff (tiffcp 4.x: `-c g4`; `-c g3:2d:fill -f lsb2msb -r 9`; and PIL's
# `compression='tiff_ccitt'`) from the page the PAGE RECIPE below draws.  The
# expected pixels come from the recipe itself, never from a decoder, so
# nothing under test computes its own answer.  The page is built to reach the
# whole code space: all 64 terminating and 27 make-up codes of each colour,
# all 13 shared extended make-up codes (runs up to 2600), and every 2-D mode
# (V0, VR1-3, VL1-3, horizontal, pass) — measured with an instrumented build
# when the fixtures were made.  Between them the three files cover 1-D rows
# without EOLs and byte-aligned (MH), EOL-framed 2-D rows with fill bits,
# LSB-first FillOrder and 9-row strips (G3), and pure MMR (G4).
#
# WHEN libtiff IS INSTALLED (tiffcp + PIL), a second, larger matrix is
# generated here from a random page: G3 1-D, 1-D+fill, 2-D, 2-D+fill, G4 in
# 16-row strips, G4 LSB-first, G4 with Photometric WhiteIsZero, and PIL's own
# group3 / group4 / tiff_ccitt writers, each compared with PIL's decode of the
# uncompressed original.  When libtiff is absent that arm prints SKIP — a skip
# is not a pass.
#
# SAFETY.  image.c is built with -fsanitize=address,undefined and fed every
# fixture truncated at 64 points and with 300 single-bit flips each; every
# input must either decode or fail with a message, with no sanitizer report.
#
# Story: 135.5a

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
SEEDS="${REPO_ROOT}/test/fuzz/seeds/image"
CC="${CC:-cc}"

PASS=0
FAIL=0
SKIP=0
ok()   { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad()  { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
skip() { echo "  SKIP: $1"; SKIP=$((SKIP + 1)); }
finish() {
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
    [ "${FAIL}" -eq 0 ]
    exit $?
}

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c156_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C156: TIFF CCITT G3/G4 decode (MH, T.4 1-D/2-D, T.6) against libtiff-written fixtures"
echo "--------------------------------------"

for f in ccitt_g4 ccitt_g3_2d ccitt_mh; do
    if [ -s "${SEEDS}/${f}.tif" ]; then cp "${SEEDS}/${f}.tif" "${WORK}/"; ok "fixture ${f}.tif is tracked"
    else bad "fixture ${f}.tif is missing from test/fuzz/seeds/image"; fi
done

# ── The page recipe and the hand-made error fixtures ─────────────────────────
cat > recipe.py <<'PY'
# Deterministic bilevel test page for CCITT (135.5a).  1 = black.
# DO NOT EDIT without regenerating test/fuzz/seeds/image/ccitt_*.tif.
W = 2600
def page():
    rows = []
    for j in range(1, 41):
        L = 64 * j + (j * 7) % 64
        rows.append(([0] * L + [1] * W)[:W])            # white make-up 64j..
        rows.append(([0] + [1] * L + [0] * W)[:W])      # black make-up 64j..
    rows.append([0] * W)
    rows.append([1] * W)
    shifts = [0, 1, -1, 2, -2, 3, -3, 1, 3, -3, -1, 2, 0, -2, 3, -1]
    edges = [100 + 90 * k for k in range(25)]
    for i, s in enumerate(shifts):
        edges = [e + (s if k % 2 == 0 else -s) for k, e in enumerate(edges)]
        r = [0] * W
        for k in range(0, len(edges) - 1, 2):
            if (i + k) % 5 == 0:
                continue
            for x in range(edges[k], min(W, edges[k] + 20 + (k % 7))):
                r[x] = 1
        for x in range(edges[k % 25] + 5, edges[k % 25] + 8):
            r[x] ^= 1
        rows.append(r)
    r = []; c = 1; n = 1
    while len(r) < W:
        r += [c] * n; c ^= 1; n = n + 1 if n < 63 else 1
    rows.append(r[:W])
    r = [0]
    for n in range(0, 64):
        r += [1] * n + [0] * n
    rows.append((r + [0] * W)[:W])
    r = [0]
    for n in range(63, -1, -1):
        r += [1] * n + [0] * n
    rows.append((r + [0] * W)[:W])
    return rows
PY

python3 - "${WORK}" <<'PY'
import os, struct, sys
W = sys.argv[1]
sys.path.insert(0, W)
from recipe import page, W as PW
rows = page()
open(os.path.join(W, 'expect.gray'), 'wb').write(
    bytes(0 if v else 255 for r in rows for v in r))
open(os.path.join(W, 'expect.dims'), 'w').write('%d %d' % (PW, len(rows)))

SHORT, LONG = 3, 4
def tiff(w, h, comp, strip, photo=0, extra=()):
    """Minimal little-endian one-strip bilevel TIFF.  Shares no code with image.c."""
    out = bytearray(b'II' + struct.pack('<HI', 42, 8))
    ents = [(256, LONG, 1, w), (257, LONG, 1, h), (258, SHORT, 1, 1),
            (259, SHORT, 1, comp), (262, SHORT, 1, photo), (273, LONG, 1, 0),
            (277, SHORT, 1, 1), (278, LONG, 1, h), (279, LONG, 1, len(strip))]
    ents += list(extra); ents.sort()
    ifd_len = 2 + 12 * len(ents) + 4
    so = 8 + ifd_len
    out += struct.pack('<H', len(ents))
    for tag, ty, cnt, val in ents:
        if tag == 273: val = so
        out += struct.pack('<HHI', tag, ty, cnt)
        out += struct.pack('<HH', val, 0) if ty == SHORT else struct.pack('<I', val)
    out += struct.pack('<I', 0)
    return bytes(out) + strip

# T.6 extension code 0000001 111 = uncompressed mode, at the start of row 0.
open(os.path.join(W, 'uncomp.tif'), 'wb').write(tiff(16, 1, 4, bytes([0x03, 0xC0, 0, 0])))
# A strip that ends mid-page: the first 40 bytes of the libtiff G4 file's
# first strip, declared as that strip's full height.
g4 = open(os.path.join(W, 'ccitt_g4.tif'), 'rb').read()
so = None
# find the StripOffsets entry of the (single) IFD
ifd = struct.unpack_from('<I', g4, 4)[0] if g4[:2] == b'II' else struct.unpack_from('>I', g4, 4)[0]
be = g4[:2] == b'MM'
n = struct.unpack_from('>H' if be else '<H', g4, ifd)[0]
for i in range(n):
    tag, ty, cnt, val = struct.unpack_from('>HHII' if be else '<HHII', g4, ifd + 2 + 12 * i)
    if tag == 273:   # the first strip's offset (an array when there are several)
        so = val if cnt == 1 else struct.unpack_from('>I' if be else '<I', g4, val)[0]
    if tag == 278: rps = val
open(os.path.join(W, 'short.tif'), 'wb').write(
    tiff(2600, rps, 4, g4[so:so + 40], photo=1))
# A CCITT page that is not bilevel: 8 bits per sample.
open(os.path.join(W, 'notbilevel.tif'), 'wb').write(
    tiff(4, 1, 4, b'\x80\x00', extra=()).replace(
        struct.pack('<HHIHH', 258, SHORT, 1, 1, 0), struct.pack('<HHIHH', 258, SHORT, 1, 8, 0)))
PY
[ -s expect.gray ] && [ -s uncomp.tif ] || { bad "fixtures were not produced"; finish; }

# ── Optional: the libtiff matrix ─────────────────────────────────────────────
BIG=""
if command -v tiffcp >/dev/null 2>&1 && python3 -c "import PIL" >/dev/null 2>&1; then
    if python3 - "${WORK}" <<'PY' 2>big.log
import os, random, subprocess, sys
from PIL import Image
W = sys.argv[1]; os.chdir(W)
random.seed(1355)
PW, PH = 2900, 64
rows = []
for y in range(PH):
    k = y % 4
    if k == 0:   row = [255 if random.random() < 0.5 else 0 for _ in range(PW)]
    elif k == 1: row = [0 if random.random() < 0.02 else 255 for _ in range(PW)]
    elif k == 2:
        s = random.choice([-3, -2, -1, 0, 1, 2, 3]); p = rows[-1]
        row = (p[-s:] + p[:-s]) if s else p[:]
    else:
        a = random.randrange(0, PW); row = [255] * a + [0] * (PW - a)
    rows.append(row[:PW])
im = Image.new('L', (PW, PH)); im.putdata([v for r in rows for v in r]); im = im.convert('1')
im.save('big_src.tif', compression='raw')
ref = Image.open('big_src.tif').convert('L').tobytes()
open('big.expect', 'wb').write(ref)
open('big.expect_inv', 'wb').write(bytes(255 - b for b in ref))
for name, args in [('big_g3_1d', ['-c', 'g3:1d']), ('big_g3_1d_fill', ['-c', 'g3:1d:fill']),
                   ('big_g3_2d', ['-c', 'g3:2d']), ('big_g3_2d_fill', ['-c', 'g3:2d:fill', '-r', '7']),
                   ('big_g4_strips', ['-c', 'g4', '-r', '16']), ('big_g4_lsb', ['-c', 'g4', '-f', 'lsb2msb'])]:
    subprocess.check_call(['tiffcp'] + args + ['big_src.tif', name + '.tif'])
subprocess.check_call(['tiffcp', '-c', 'g4', 'big_src.tif', 'big_g4_wiz.tif'])
# Photometric flipped on an otherwise identical G4 page: the same bits must
# now read as the inverse image.
import struct
b = bytearray(open('big_g4_wiz.tif', 'rb').read())
be = b[:2] == b'MM'; fmt = '>' if be else '<'
ifd = struct.unpack_from(fmt + 'I', b, 4)[0]
for i in range(struct.unpack_from(fmt + 'H', b, ifd)[0]):
    e = ifd + 2 + 12 * i
    if struct.unpack_from(fmt + 'H', b, e)[0] == 262:
        struct.pack_into(fmt + 'H', b, e + 8, 1 - struct.unpack_from(fmt + 'H', b, e + 8)[0])
open('big_g4_wiz.tif', 'wb').write(bytes(b))
src = Image.open('big_src.tif')
src.save('big_pil_g3.tif', compression='group3')
src.save('big_pil_g4.tif', compression='group4')
src.save('big_pil_mh.tif', compression='tiff_ccitt')
PY
    then
        BIG="big_g3_1d big_g3_1d_fill big_g3_2d big_g3_2d_fill big_g4_strips big_g4_lsb big_g4_wiz big_pil_g3 big_pil_g4 big_pil_mh"
        ok "libtiff matrix generated (10 variants of a random 2900x64 page)"
    else
        bad "libtiff is installed but the matrix could not be generated: $(tail -1 big.log)"
    fi
else
    skip "libtiff matrix (needs tiffcp and PIL) — the tracked fixtures above still run"
fi

# ── A toke program decodes every fixture ─────────────────────────────────────
NAMES="ccitt_g4 ccitt_g3_2d ccitt_mh uncomp short notbilevel ${BIG}"
{
cat <<EOF
m=c156;
i=io:std.io;
i=file:std.file;
i=str:std.str;
i=image:std.image;

f=one(name:str):i64{
  let raw = mt file.readbytes(str.concat("${WORK}/"; str.concat(name; ".tif"))) {\$ok:d d;\$err:e @()};
  let empty = \$imgbuf{width:0;height:0;channels:0;data:@()};
  let msg = mt image.tiffdecode(raw; 0) {\$ok:i "";\$err:e e};
  io.println(str.concat(name; str.concat(".err="; msg)));
  let img = mt image.tiffdecode(raw; 0) {\$ok:i i;\$err:e empty};
  io.println(str.concat(name; str.concat(".dims="; str.concat(str.fromint(img.width as i64); str.concat("x"; str.concat(str.fromint(img.height as i64); str.concat("x"; str.fromint(img.channels as i64))))))));
  let wrote = mt file.writebytes(str.concat("${WORK}/"; str.concat(name; ".out")); img.data) {\$ok:v v;\$err:e false};
  <0
};

f=main():i64{
EOF
for n in ${NAMES}; do echo "  one(\"${n}\");"; done
cat <<'EOF'
  <0
};
EOF
} > c156.tk

if "${TKC}" --allow-all -o c156.bin c156.tk >build.log 2>&1; then
    ok "a toke program compiles image.tiffdecode over the CCITT fixtures"
else
    bad "c156.tk did not compile"
    sed 's/^/      /' build.log | head -12
    finish
fi
if ./c156.bin > run.log 2>&1; then ok "the toke program ran to completion"
else bad "the toke program exited non-zero"; sed 's/^/      /' run.log | head -8; fi
grep -q '^ccitt_g4.dims=' run.log || { bad "no usable output: [$(head -3 run.log)]"; finish; }

python3 - "${WORK}" "${BIG}" > verdicts.txt <<'PY'
import os, sys
W = sys.argv[1]; big = sys.argv[2].split()
log = {}
for line in open(os.path.join(W, 'run.log')):
    k, _, v = line.rstrip('\n').partition('=')
    if _ and not (k.endswith('.err') and v == ''): log[k] = v
def rd(n):
    p = os.path.join(W, n)
    return open(p, 'rb').read() if os.path.exists(p) else None
def say(c, m): print(("OK " if c else "NO ") + m)
pw, ph = open(os.path.join(W, 'expect.dims')).read().split()
want = rd('expect.gray')
for n, what in [('ccitt_g4', 'T.6 / G4 (MMR)'), ('ccitt_g3_2d', 'T.4 2-D, EOL fill bits, LSB-first, 9-row strips'),
                ('ccitt_mh', 'Modified Huffman (compression 2)')]:
    dims = log.get(n + '.dims')
    say(dims == '%sx%sx1' % (pw, ph), "%s: decodes to a %sx%s grey plane (got %r%s)"
        % (n, pw, ph, dims, (', err ' + log[n + '.err']) if n + '.err' in log else ''))
    got = rd(n + '.out')
    diff = -1 if got is None or len(got) != len(want) else sum(a != b for a, b in zip(got, want))
    say(diff == 0, "%s (%s): every one of the %d pixels matches the recipe (%d differ)"
        % (n, what, len(want), diff))
say('CCITT' in log.get('uncomp.err', '') and 'uncompressed mode' in log.get('uncomp.err', ''),
    "T.6 uncompressed mode is refused by name (got %r)" % log.get('uncomp.err'))
say('truncated' in log.get('short.err', ''),
    "a strip that ends mid-page is an error naming the CCITT strip, not a short image (got %r)"
    % log.get('short.err'))
say('bilevel' in log.get('notbilevel.err', ''),
    "a CCITT page with 8 bits per sample is refused as not bilevel (got %r)" % log.get('notbilevel.err'))
if big:
    exp, inv = rd('big.expect'), rd('big.expect_inv')
    for n in big:
        e = inv if n.endswith('wiz') else exp
        got = rd(n + '.out')
        diff = -1 if got is None or len(got) != len(e) else sum(a != b for a, b in zip(got, e))
        say(diff == 0, "libtiff %s: all %d pixels match PIL's decode of the uncompressed page (%d differ%s)"
            % (n, len(e), diff, (', err ' + log[n + '.err']) if n + '.err' in log else ''))
PY
while IFS= read -r line; do
    case "${line}" in "OK "*) ok "${line#OK }" ;; "NO "*) bad "${line#NO }" ;; esac
done < verdicts.txt
[ -s verdicts.txt ] || bad "the pixel checks produced no verdicts"

# ── ASan + UBSan over truncations and bit flips ──────────────────────────────
cat > san.c <<'EOF'
#include "image.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static unsigned char *slurp(const char *p, long *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); *n = ftell(f); rewind(f);
    unsigned char *b = malloc((size_t)*n);
    if (fread(b, 1, (size_t)*n, f) != (size_t)*n) { fclose(f); free(b); return NULL; }
    fclose(f); return b;
}
static unsigned long ok_n, err_n;
static void one(const unsigned char *b, long n) {
    unsigned char *c = malloc(n ? (size_t)n : 1);   /* exact-size copy: ASan sees any over-read */
    memcpy(c, b, (size_t)n);
    ImgResult r = image_tiff_decode(c, (uint64_t)n, 0);
    if (r.is_err) { if (!r.err_msg || !r.err_msg[0]) { printf("EMPTY-MESSAGE\n"); exit(3); } err_n++; }
    else { ok_n++; image_buf_free(&r.ok); }
    free(c);
}
int main(int argc, char **argv) {
    unsigned long seed = 1355;
    for (int a = 1; a < argc; a++) {
        long n; unsigned char *b = slurp(argv[a], &n);
        if (!b) { printf("cannot read %s\n", argv[a]); return 2; }
        one(b, n);
        for (int k = 1; k <= 64; k++) one(b, n * k / 65);
        for (int k = 0; k < 300; k++) {
            seed = seed * 6364136223846793005ul + 1442695040888963407ul;
            long at = (long)((seed >> 33) % (unsigned long)n);
            b[at] ^= (unsigned char)(1u << ((seed >> 29) & 7));
            one(b, n);
            b[at] ^= (unsigned char)(1u << ((seed >> 29) & 7));
        }
        free(b);
    }
    printf("decoded %lu, refused %lu\n", ok_n, err_n);
    return 0;
}
EOF
if "${CC}" -std=c99 -g -O1 -fsanitize=address,undefined -fno-sanitize-recover=all \
        -iquote "${REPO_ROOT}/src/stdlib" -o san san.c "${REPO_ROOT}/src/stdlib/image.c" >san_build.log 2>&1; then
    files="ccitt_g4.tif ccitt_g3_2d.tif ccitt_mh.tif uncomp.tif short.tif"
    for n in ${BIG}; do files="${files} ${n}.tif"; done
    if out=$(ASAN_OPTIONS="detect_leaks=0" ./san ${files} 2>&1); then
        ok "ASan+UBSan: every fixture, 64 truncations and 300 bit flips each, no report ($(echo "${out}" | tail -1))"
    else
        bad "ASan+UBSan reported on a mutated CCITT input"
        echo "${out}" | head -20 | sed 's/^/      /'
    fi
else
    skip "sanitizer build unavailable on this host ($(head -1 san_build.log))"
fi

finish
