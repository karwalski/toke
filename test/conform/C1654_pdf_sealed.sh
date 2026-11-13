#!/usr/bin/env bash
# C1654_pdf_sealed.sh -- std.pdf hands toke sealed strings, and pdf.c,
# pdftext.c and pdf_glue.c read with tk_strlen (story 127.295, S1 step 5
# tranche 14; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen.  pdf_glue.c makes the copies
# toke keeps -- a $textrun's .text and pdf.pagetext's result -- and they are
# sealed now (tks_dup).  pdf.c's run text and pdf_page_text's buffer stay
# malloc'd: the page owns the first (pdf_page_free), the glue frees the
# second after copying it.
#
# THE FIXTURE is a one-page PDF written here, byte by byte, with a correct
# xref table: Helvetica (WinAnsiEncoding), "Hello sealed" at y=720 and
# "second line" at y=700.  C032 is the module's real fixture test, but its
# fixtures need reportlab/pikepdf/Pillow and it skips without them; this one
# needs nothing.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1), through the glue the
#      compiler calls: tk_pdf_openfile_w opens it; tk_pdf_pagetext_w is
#      sealed and equal to "Hello sealed\nsecond line"; tk_pdf_runs_w gives
#      two runs whose .text slots are sealed and equal to the two strings.
#   2. toke, under TK_STR_VERIFY=1: pdf.pagetext and pdf.runs .text and .len.
#
# Negative control: C1654_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base e810734c's src/stdlib + stdlib: 2 passed, 2 failed -- the open and
#   toke cases pass there (an unsealed string read with strlen was always
#   correct); pagetext and the runs' .text report "not sealed".
# Measured: this branch 4 passed (3 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1654_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1654_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1654: std.pdf results sealed; pdf.c/pdftext.c/pdf_glue.c read with tk_strlen (127.295)"
echo "-------------------------------------------------------------------------------------"

# ── the fixture ──────────────────────────────────────────────────────────────
CONTENT='BT /F1 12 Tf 72 720 Td (Hello sealed) Tj ET
BT /F1 12 Tf 72 700 Td (second line) Tj ET
'
O1='<< /Type /Catalog /Pages 2 0 R >>'
O2='<< /Type /Pages /Kids [3 0 R] /Count 1 >>'
O3='<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792] /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R >>'
O4="$(printf '<< /Length %d >>\nstream\n%sendstream' "${#CONTENT}" "${CONTENT}")"
O5='<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>'
printf '%%PDF-1.4\n' > t.pdf
OFFS=()
i=1
for o in "${O1}" "${O2}" "${O3}" "${O4}" "${O5}"; do
    OFFS+=("$(wc -c < t.pdf | tr -d ' ')")
    printf '%d 0 obj\n%s\nendobj\n' "${i}" "${o}" >> t.pdf
    i=$((i + 1))
done
XREF="$(wc -c < t.pdf | tr -d ' ')"
{
    printf 'xref\n0 6\n0000000000 65535 f \n'
    for off in "${OFFS[@]}"; do printf '%010d 00000 n \n' "${off}"; done
    printf 'trailer\n<< /Size 6 /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n' "${XREF}"
} >> t.pdf

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "capabilities.h"
#include "tk_runtime.h"
int64_t tk_pdf_openfile_w(int64_t path);
int64_t tk_pdf_pagetext_w(int64_t handle, int64_t page);
int64_t tk_pdf_runs_w(int64_t handle, int64_t page);
int64_t tk_pdf_close_w(int64_t handle);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.60s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
int main(void)
{
    char a0[] = "seal", a1[] = "--allow-read";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    int64_t d = tk_pdf_openfile_w(I("t.pdf"));
    CHECK(d != 0 && tk_current_error == 0, "pdf.openfile opens the fixture");
    if (!d) return 1;
    CHECK(sealed_eq(P(tk_pdf_pagetext_w(d, 1)), "Hello sealed\nsecond line"),
          "pdf.pagetext: sealed, reading order");
    int64_t a = tk_pdf_runs_w(d, 1);
    int ok = a && ((int64_t *)(intptr_t)a)[-1] == 2;
    if (ok) {
        const int64_t *r0 = (const int64_t *)(intptr_t)((int64_t *)(intptr_t)a)[0];
        const int64_t *r1 = (const int64_t *)(intptr_t)((int64_t *)(intptr_t)a)[1];
        ok = sealed_eq(P(r0[0]), "Hello sealed") & sealed_eq(P(r1[0]), "second line");
    } else printf("        (runs: %lld)\n", a ? (long long)((int64_t *)(intptr_t)a)[-1] : -1LL);
    CHECK(ok, "pdf.runs: two runs, each .text sealed");
    tk_pdf_close_w(d);
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects pdf -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -20
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=pdf:std.pdf;
f=show(d:$pdfdoc):i64{
  mt pdf.pagetext(d;1) {$ok:t io.println("text.len=\(t.len)"); $err:e io.println("pagetext failed")};
  mt pdf.runs(d;1) {$ok:rs io.println("runs=\(rs.len) \(rs.get(0).text) \(rs.get(0).text.len) \(rs.get(1).text.len)"); $err:e io.println("runs failed")};
  pdf.close(d);
  <0
};
f=main():i64{
  mt pdf.openfile("t.pdf") {$ok:d show(d); $err:e io.println("open failed")};
  <0
};
TOKE
WANT='text.len=24
runs=2 Hello sealed 12 11'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: pdf.pagetext and pdf.runs .text and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "-------------------------------------------------------------------------------------"
echo "C1654: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
