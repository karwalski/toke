#!/usr/bin/env bash
# C1562_ocr_sealed.sh -- std.ocr hands toke sealed strings, and ocr.c reads
# with tk_strlen (story 127.295, S1 step 5 tranche 11; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# ocr.languages() used to hand toke ocr.c's static g_lang_list itself, which
# ocr_languages() clears and refills on every call (ocr.setlanguages calls
# it too): a string the program held was rewritten under it.  The glue now
# hands over a sealed copy.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen).  The driver #includes ocr_glue.c to reach
#      its static helpers; the rest of std.ocr's link set comes from
#      stdlib_link_set.sh --objects --except ocr_glue.c:
#        - build_run: a run's text slot is sealed with the exact bytes
#          (UTF-8 included), and a NULL text is a sealed "";
#        - tk_ocr_languages_w: sealed, a copy (not ocr.c's buffer), equal to
#          ocr_languages(), and unchanged after ocr_set_languages and
#          another ocr_languages() call ("" sealed when there is no engine);
#        - with an engine and Pillow (else these cases are skipped, said
#          so): tk_ocr_runs_w's every run text and tk_ocr_text_w's join are
#          sealed, and the join holds "ACME BANK"; a blank page's text is a
#          sealed "".  (ocr.runsfile/textfile share dup_str and build_runs;
#          they are fs.read sinks this driver does not grant.)
#   2. toke, under TK_STR_VERIFY=1: ocr.languages() `.len` equals the
#      length of its text, and ocr.engine() is "vision" or "none".
#
# Exact recognised strings and geometry are C033's job; this gate is about
# ownership and the length header only.
#
# Negative control: C1562_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base effa0ad8's src/stdlib + stdlib: 1 passed, 3 failed (macOS with
#   Vision and Pillow) -- every producer case reports "not sealed" or, for
#   languages, "not a copy"; the toke case passes there.
# Measured: this branch 4 passed (3 C + 1 toke) on macOS with Vision and
# Pillow.
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1562_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1562_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1562: std.ocr results sealed; ocr.c reads with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

# Fixtures (optional): text.png with two known lines, blank.png.
HAVE_FIX=0
if command -v python3 >/dev/null 2>&1 && python3 -c "import PIL" >/dev/null 2>&1; then
    python3 - "${WORK}" > fx.log 2>&1 <<'PY' && HAVE_FIX=1
import os, sys
from PIL import Image, ImageDraw, ImageFont
d = sys.argv[1]
face = next((p for p in ["/System/Library/Fonts/Supplemental/Arial.ttf",
                         "/System/Library/Fonts/Supplemental/Helvetica.ttf",
                         "/Library/Fonts/Arial.ttf"] if os.path.exists(p)), None)
if face is None: sys.exit(2)
img = Image.new("RGB", (900, 220), "white")
dr = ImageDraw.Draw(img)
font = ImageFont.truetype(face, 34)
dr.text((30, 30), "ACME BANK", fill="black", font=font)
dr.text((30, 110), "Ref 998877", fill="black", font=font)
img.save(os.path.join(d, "text.png"))
Image.new("RGB", (900, 220), "white").save(os.path.join(d, "blank.png"))
PY
fi

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include "ocr_glue.c"
#include <stdio.h>
#include "tk_strhdr.h"
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.60s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int64_t alen(int64_t a) { return ((int64_t *)(intptr_t)a)[-1]; }
static int64_t aget(int64_t a, int64_t i) { return ((int64_t *)(intptr_t)a)[i]; }
/* An image file as the @(byte) array ocr.runs / ocr.text take (the *file
 * variants are fs.read sinks, and this driver grants nothing). */
static int64_t load(const char *dir, const char *name)
{
    char path[1200];
    snprintf(path, sizeof path, "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    static uint8_t buf[2][1 << 20];
    static int k;
    uint8_t *b = buf[k++ & 1];
    size_t n = fread(b, 1, sizeof buf[0], f);
    fclose(f);
    return tk_bytes_pack(b, n);
}
int main(int argc, char **argv)
{
    int fix = argc > 2 && argv[2][0] == '1';
    TkOcrRun r = { 0 };
    r.text = (char *)"caf\xc3\xa9 r\xc3\xa9" "f 42";
    int64_t *slot = (int64_t *)(intptr_t)build_run(&r);
    CHECK(slot && sealed_eq(P(slot[0]), "caf\xc3\xa9 r\xc3\xa9" "f 42") && sealed_eq(P(dup_str(NULL)), ""),
          "build_run: run text sealed with the exact bytes; a NULL text is a sealed \"\"");

    const char *l1 = P(tk_ocr_languages_w());
    char snap[1024];
    snprintf(snap, sizeof snap, "%s", l1);
    int copy = l1 != ocr_languages() && strcmp(l1, ocr_languages()) == 0;
    if (!copy) printf("        (not a copy of ocr_languages())\n");
    ocr_set_languages("en-US");
    (void)ocr_languages();
    ocr_set_languages(NULL);
    CHECK(sealed_eq(l1, snap) && copy && (ocr_is_available() ? strstr(l1, "en-US") != NULL : l1[0] == '\0'),
          "tk_ocr_languages_w: a sealed copy, unchanged by later setlanguages/languages calls");

    if (!ocr_is_available() || !fix) {
        printf("  SKIP: recognition cases (engine %s, fixtures %s)\n",
               ocr_is_available() ? "present" : "absent", fix ? "present" : "absent");
        return bad;
    }
    int64_t img = load(argv[1], "text.png"), blank = load(argv[1], "blank.png");
    int64_t runs = img ? tk_ocr_runs_w(img) : 0;
    int ok = runs && alen(runs) >= 1;
    for (int64_t i = 0; ok && i < alen(runs); i++)
        ok = sealed_eq(P(((int64_t *)(intptr_t)aget(runs, i))[0]), NULL);
    const char *t = img ? P(tk_ocr_text_w(img)) : NULL;
    const char *b = blank ? P(tk_ocr_text_w(blank)) : NULL;
    CHECK(ok && sealed_eq(t, NULL) && strstr(t, "ACME BANK") != NULL && sealed_eq(b, ""),
          "tk_ocr_runs_w run texts, tk_ocr_text_w's join and a blank page's \"\": sealed");
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list; ocr_glue.c is #included above.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --except ocr_glue.c --objects ocr -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LFLAGS="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --flags ocr)" || LFLAGS=""
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
# shellcheck disable=SC2206
LF=(${LFLAGS})
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" ${LF[@]+"${LF[@]}"} -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${WORK}" "${HAVE_FIX}" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL|SKIP)|^        ' seal.out | head -20
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=o:std.ocr;
f=main():i64{
  let l=o.languages();
  io.println("langs=\(l.len)");
  io.println("[\(l)]");
  io.println("engine=\(o.engine())");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    n="$(sed -n 's/^langs=//p' p.out)"
    txt="$(sed -n '2p' p.out)"
    want=$(( ${#txt} - 2 ))
    if [ "${rc}" -eq 0 ] && [ -n "${n}" ] && [ "${n}" -eq "${want}" ] &&
       grep -a -q -x -E 'engine=(vision|none)' p.out; then
        pass "toke: ocr.languages() .len (${n}) matches its text, engine named, under TK_STR_VERIFY=1"
    else
        fail "toke (rc=${rc}): '$(head -3 p.out | tr '\n' '|' | head -c 200)'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1562: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
