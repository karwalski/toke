#!/usr/bin/env bash
# C1561_xlsx_sealed.sh -- std.xlsx hands toke sealed strings, and xlsx.c reads
# with tk_strlen (story 127.295, S1 step 5 tranche 11; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# THE FIXTURE is a minimal workbook written here with Python's zipfile (no
# openpyxl needed; C031 is the gate on real writers' output): one sheet named
# "Data" with a shared-string cell, an inline-string cell, a number, a
# boolean, and a hole at B2 that the rectangle fills.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), through the glue the compiler calls:
#        - tk_xlsx_sheets_w: every sheet name sealed, exact bytes;
#        - tk_xlsx_rows_w: every str slot of every $xlsxcell (ref, celltype,
#          raw, value) sealed with the exact bytes, holes included (an empty
#          raw/value is a sealed "");
#        - tk_xlsx_datefromserial_w: a date and the junk "" both sealed;
#        - a sealed sheet name passed back in reads the same sheet.
#   2. toke, under TK_STR_VERIFY=1: xlsx.openfile -> sheets -> rows, `.len`
#      of a cell value, and datefromserial.
#
# Negative control: C1561_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base effa0ad8's src/stdlib + stdlib: 2 passed, 3 failed -- open and the
#   toke case pass there (an unsealed string read with strlen was always
#   correct); every producer case reports "not sealed".
# Measured: this branch 5 passed (4 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1561_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found (it writes the fixture workbook)"
    echo "CONFORM-SKIP: python3 not installed"
    exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1561_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1561: std.xlsx results sealed; xlsx.c reads with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

python3 - "${WORK}/book.xlsx" > fx.log 2>&1 <<'PY'
import sys, zipfile
ct = ('<?xml version="1.0" encoding="UTF-8" standalone="yes"?>'
      '<Types xmlns="http://schemas.openxmlformats.org/package/2006/content-types">'
      '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
      '<Default Extension="xml" ContentType="application/xml"/>'
      '<Override PartName="/xl/workbook.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml"/>'
      '<Override PartName="/xl/worksheets/sheet1.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml"/>'
      '<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/>'
      '</Types>')
rels = ('<?xml version="1.0" encoding="UTF-8"?>'
        '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
        '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
        '</Relationships>')
wb = ('<?xml version="1.0" encoding="UTF-8"?>'
      '<workbook xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
      'xmlns:r="http://schemas.openxmlformats.org/officeDocument/2006/relationships">'
      '<sheets><sheet name="Data" sheetId="1" r:id="rId1"/></sheets></workbook>')
wbrels = ('<?xml version="1.0" encoding="UTF-8"?>'
          '<Relationships xmlns="http://schemas.openxmlformats.org/package/2006/relationships">'
          '<Relationship Id="rId1" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
          '<Relationship Id="rId2" Type="http://schemas.openxmlformats.org/officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
          '</Relationships>')
sst = ('<?xml version="1.0" encoding="UTF-8"?>'
       '<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" count="1" uniqueCount="1">'
       '<si><t>Description &amp; more</t></si></sst>')
sh = ('<?xml version="1.0" encoding="UTF-8"?>'
      '<worksheet xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main"><sheetData>'
      '<row r="1"><c r="A1" t="s"><v>0</v></c><c r="B1" t="inlineStr"><is><t>inline</t></is></c></row>'
      '<row r="2"><c r="A2"><v>42.5</v></c></row>'
      '<row r="3"><c r="A3" t="b"><v>1</v></c><c r="B3"><v>7</v></c></row>'
      '</sheetData></worksheet>')
with zipfile.ZipFile(sys.argv[1], "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("[Content_Types].xml", ct)
    z.writestr("_rels/.rels", rels)
    z.writestr("xl/workbook.xml", wb)
    z.writestr("xl/_rels/workbook.xml.rels", wbrels)
    z.writestr("xl/sharedStrings.xml", sst)
    z.writestr("xl/worksheets/sheet1.xml", sh)
PY
if [ ! -s "${WORK}/book.xlsx" ]; then
    fail "fixture: $(head -3 fx.log)"
    echo "C1561: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "bytes_rt.h"
int64_t tk_xlsx_open_w(int64_t data);
int64_t tk_xlsx_sheets_w(int64_t handle);
int64_t tk_xlsx_rows_w(int64_t handle, int64_t sheet);
int64_t tk_xlsx_datefromserial_w(int64_t serial, int64_t system);
int64_t tk_xlsx_close_w(int64_t handle);
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
/* ref, celltype, raw, value for the 3x2 rectangle, row-major. */
static const char *want[6][4] = {
    { "A1", "str",   "0",      "Description & more" },
    { "B1", "str",   "inline", "inline" },
    { "A2", "num",   "42.5",   "42.5" },
    { "B2", "empty", "",       "" },
    { "A3", "bool",  "1",      "true" },
    { "B3", "num",   "7",      "7" },
};
static int rows_ok(int64_t rows)
{
    if (!rows || alen(rows) != 3) { printf("        (rows: %lld)\n", rows ? (long long)alen(rows) : -1LL); return 0; }
    int ok = 1;
    for (int r = 0; r < 3; r++) {
        int64_t row = aget(rows, r);
        if (alen(row) != 2) { printf("        (row %d width %lld)\n", r, (long long)alen(row)); return 0; }
        for (int c = 0; c < 2; c++) {
            int64_t *cell = (int64_t *)(intptr_t)aget(row, c);
            const char **w = want[r * 2 + c];
            int k = sealed_eq(P(cell[0]), w[0]) & sealed_eq(P(cell[3]), w[1]) &
                    sealed_eq(P(cell[4]), w[2]) & sealed_eq(P(cell[5]), w[3]);
            if (!k) printf("        (cell %s)\n", w[0]);
            ok &= k && cell[1] == r + 1 && cell[2] == c + 1;
        }
    }
    return ok;
}
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    FILE *f = fopen(argv[1], "rb");
    if (!f) return 2;
    static uint8_t buf[1 << 16];
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    int64_t wb = tk_xlsx_open_w(tk_bytes_pack(buf, n));
    CHECK(wb != 0, "tk_xlsx_open_w opens the fixture");
    if (!wb) return bad;
    int64_t sheets = tk_xlsx_sheets_w(wb);
    CHECK(sheets && alen(sheets) == 1 && sealed_eq(P(aget(sheets, 0)), "Data"),
          "tk_xlsx_sheets_w: the sheet name is sealed");
    CHECK(rows_ok(tk_xlsx_rows_w(wb, I(tks_strdup("Data")))) &&
          rows_ok(tk_xlsx_rows_w(wb, aget(sheets, 0))),
          "tk_xlsx_rows_w: every str slot of every cell sealed, holes included (sealed and returned sheet names)");
    CHECK(sealed_eq(P(tk_xlsx_datefromserial_w(I(tks_strdup("60")), I("1900"))), "1900-02-29") &&
          sealed_eq(P(tk_xlsx_datefromserial_w(I("59.25"), I(tks_strdup("1900")))), "1900-02-28T06:00:00") &&
          sealed_eq(P(tk_xlsx_datefromserial_w(I("junk"), I("1900"))), ""),
          "tk_xlsx_datefromserial_w: a date, a time and junk's \"\" all sealed");
    tk_xlsx_close_w(wb);
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects xlsx -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
# The module's extra link flags (frameworks, -l...) come from the same
# dependency table, read straight from `tkc --emit-deps` (the part after
# the first "---"): stdlib_link_set.sh --flags would be a call outside
# --objects, which C703 case 12 forbids (127.408).
printf 'm=lf;\ni=x:std.xlsx;\nf=main():i64{<0};\n' > "${WORK}/lflags.tk"
LFLAGS="$("${TKC}" --emit-deps "${WORK}/lflags.tk" 2>/dev/null \
        | awk 'seen == 1 && /^---$/ { exit } seen == 1 && NF { print } /^---$/ { seen = 1 }')" \
    || LFLAGS=""
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
LF=(); while IFS= read -r l; do [ -n "$l" ] && LF+=("$l"); done <<< "${LFLAGS}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" ${LF[@]+"${LF[@]}"} -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${WORK}/book.xlsx" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=x:std.xlsx;
f=body(wb:$xlsxbook):i64{
  let sh=x.sheets(wb);
  let r=x.rows(wb;sh.get(0));
  mt r {
    $ok:rows show(rows.get(0).get(0).value;sh.get(0));
    $err:e io.println("rows failed")
  };
  let d=x.datefromserial("60";"1900");
  io.println("d=\(d) \(d.len)");
  x.close(wb);
  <0
};
f=show(v:str;name:str):i64{
  io.println("v=[\(v)] \(v.len) \(name.len)");
  <0
};
f=main():i64{
  let w=x.openfile("book.xlsx");
  mt w {
    $ok:b body(b);
    $err:e io.println("open failed")
  };
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    if [ "${rc}" -eq 0 ] && grep -a -q -x -F -- "v=[Description & more] 18 4" p.out &&
       grep -a -q -x -F -- "d=1900-02-29 10" p.out; then
        pass "toke: xlsx.openfile -> sheets -> rows value, datefromserial, .len under TK_STR_VERIFY=1"
    else
        fail "toke (rc=${rc}): '$(head -3 p.out | tr '\n' '|')'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1561: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
