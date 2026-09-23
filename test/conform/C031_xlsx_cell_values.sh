#!/usr/bin/env bash
# C031_xlsx_cell_values.sh — reading cell VALUES out of an XLSX (story 135.4).
#
# std.xlsx is a parser over std.zip, not a second vendored library: an XLSX is
# a zip of XML, ADR-0015 leaves the toolchain C99 only, and the format's
# difficulty is not in the XML anyway. It is in three details that each
# produce a plausible WRONG ANSWER rather than a failure, which is why this
# script asserts on RESOLVED CELL VALUES throughout and never on "it parsed".
#
# THE FIXTURES ARE WRITTEN BY AN INDEPENDENT ENCODER. openpyxl writes the
# workbooks; it was written to interoperate with Excel, not with this module,
# so it does not share this parser's misunderstandings. Where openpyxl cannot
# produce what is needed, the gap is closed and then CHECKED WITH openpyxl'S
# OWN READER rather than believed — see case 1.
#
# THE THREE TRAPS, AND THE ASSERTION THAT CATCHES EACH:
#
#   1. SHARED STRINGS. A text cell normally holds an INDEX into
#      xl/sharedStrings.xml, not the text. A parser reading only the
#      worksheet returns small integers where the column header says
#      `Description`, and it looks CORRECT on a hand-made fixture because
#      inline text is the easy thing to write by hand.
#
#      This turned out to matter more than expected: openpyxl writes every
#      text cell INLINE (`t="inlineStr"`) and emits no sharedStrings.xml at
#      all, which was verified rather than assumed. A fixture built only by
#      openpyxl would therefore let a pool-blind parser pass. So this script
#      builds BOTH workbooks — the inline one straight from openpyxl, and a
#      pooled one re-encoded from it — and asserts the SAME resolved values
#      from each. The pooled one is validated by loading it back with
#      openpyxl, an independent DECODER, which must agree cell for cell
#      before a single toke assertion runs.
#
#      The pool is sorted, so a cell's index is not its position in reading
#      order: A1 is index 2 and C1 is index 0. A parser returning the index
#      then returns a number that is visibly not the header, and `.raw` is
#      asserted to BE that index so the resolution is proved to have
#      happened rather than inferred from the text being right.
#
#   2. DATES ARE SERIAL NUMBERS, AND THE 1900 LEAP-YEAR BUG IS REPRODUCED.
#      Excel inherited Lotus 1-2-3's belief that 1900 was a leap year, so
#      serial 60 IS 1900-02-29 — a day that never happened — and every serial
#      at or below 59 is counted from a DIFFERENT epoch than every serial at
#      or above 61. A reader that "fixes" this shifts every date on or before
#      1900-02-28 by one day, silently.
#
#      The fixture proves the encoding rather than asserting the rule from
#      memory: openpyxl wrote serial 59 for 1900-02-28 and serial 61 for
#      1900-03-01, skipping 60. Both are asserted, AND the script asserts
#      that the serials in the file really are 59 and 61 — otherwise the
#      dates could be right for the wrong reason.
#
#      `xlsx.datefromserial` then walks 58..62 directly, including the
#      phantom day itself, which a fixture cannot contain: no encoder will
#      write 1900-02-29 because no date library has such a date.
#
#      And the SAME SERIAL is read in both date systems: serial 59 is
#      1900-02-28 in a 1900 workbook and 1904-02-29 in a 1904 one (1904 was a
#      real leap year and has no phantom day). A parser that applies the bug
#      unconditionally, or never, fails one of those two lines.
#
#   3. ROWS AND COLUMNS ARE SPARSE. Cells carry references like `C7` and
#      absent cells are simply missing, so position cannot be inferred from
#      order. The fixture has a column gap (row 2 holds A, B, C and E — D is
#      absent), an interior gap (row 6 holds A and C), and a MISSING ROW
#      (row 5 does not appear at all). Every cell's `.ref`, `.row` and `.col`
#      are asserted alongside its value, so a parser that counted left to
#      right — putting E2's `true` under D — fails on the value AND on the
#      reference.
#
# WHAT IS DELIBERATELY NOT HERE: formula evaluation, styling, charts, pivot
# tables. A formula's CACHED value is asserted (case 6), because that is what
# a data-import consumer wants and evaluating formulae is an unbounded project
# with no bearing on reading a bank statement.
#
# Story: 135.4

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

WORK="$(mktemp -d /tmp/tkc_xlsx_XXXXXX)"
FIX="${WORK}/fix"
mkdir -p "${FIX}"
trap 'rm -rf "${WORK}"' EXIT

echo "C031: cell values out of an XLSX — shared strings, date serials, sparse rows"
echo "--------------------------------------"

# ════════════════════════════════════════════════════════════════════════
# 0. FIXTURES, FROM AN INDEPENDENT ENCODER
#
# 131.79: a harness failure must announce itself AS a harness failure, here,
# rather than masquerade as a std.xlsx failure two hundred lines later.
# ════════════════════════════════════════════════════════════════════════
GEN_LOG="${WORK}/generate.log"

gen_died() {
    echo "FAIL generate-fixtures: $1"
    echo "    HARNESS FAILURE, not a std.xlsx failure. The fixture workbooks"
    echo "    every assertion below is compared against were not produced, so"
    echo "    nothing below was tested."
    if [ -s "${GEN_LOG}" ]; then
        echo "    generator output:"
        sed 's/^/        /' "${GEN_LOG}"
    fi
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, $((FAIL + 1)) failed"
    exit 1
}

# SOURCE_DATE_EPOCH: the Makefile exports it as 0 for reproducible compiler
# builds (Makefile:85), and Python 3.14's zipfile honours it when stamping
# entries.  0 is 1970, which is before the 1980 DOS epoch a zip local header
# can express, so struct.pack raised inside zipfile.FileHeader and openpyxl's
# save() died -- taking every fixture with it.  This test therefore FAILED
# 100% of the time under `make` and passed 100% of the time when run by hand,
# which reads as flakiness and is not: it is a clean dependency on how the
# suite was invoked.  C007 hit the same trap and documents it; pinning the
# stamp here to the DOS epoch itself fixes it in the one place openpyxl can be
# reached, and keeps the fixtures byte-identical from run to run.
export SOURCE_DATE_EPOCH=315532800   # 1980-01-01T00:00:00Z, the DOS zero

# openpyxl is the independent encoder.  It is not a build dependency of this
# repository, so a checkout without it SKIPS rather than fails — but it skips
# LOUDLY, with a non-zero exit, because a silent skip of the only behavioural
# test for a module is how a module comes to be unprotected.
PY="${TKC_PY:-python3}"
if ! "${PY}" - <<'PY' >/dev/null 2>&1
import openpyxl
PY
then
    for cand in /tmp/xlsxvenv/bin/python "${HOME}/.venvs/xlsx/bin/python"; do
        if [ -x "${cand}" ] && "${cand}" -c 'import openpyxl' >/dev/null 2>&1; then
            PY="${cand}"
            break
        fi
    done
fi
if ! "${PY}" -c 'import openpyxl' >/dev/null 2>&1; then
    echo "SKIP: python3 has no openpyxl — the independent encoder this test's"
    echo "      evidence depends on is unavailable.  Install it with"
    echo "      'python3 -m pip install openpyxl' (a virtualenv is fine; set"
    echo "      TKC_PY to its interpreter).  Refusing to substitute a"
    echo "      hand-written fixture: a fixture written by the same hand as"
    echo "      the parser agrees with the parser's mistakes, which is the"
    echo "      one thing this test exists to rule out."
    exit 1
fi

if ! "${PY}" - "${FIX}" > "${GEN_LOG}" 2>&1 <<'PY'
import datetime, re, sys, zipfile
from openpyxl import Workbook
from openpyxl.utils.datetime import CALENDAR_MAC_1904
import openpyxl

F = sys.argv[1]

# ── the 1900-system workbook ────────────────────────────────────────────
wb = Workbook()
sh = wb.active
sh.title = "Ledger"

sh["A1"] = "Date"
sh["B1"] = "Description"
sh["C1"] = "Amount"
sh["E1"] = "Flag"                  # D1 absent: TRAP 3, in the header row

# Row 2: A B C _ E  -- the column gap.
sh["A2"] = datetime.datetime(2026, 9, 23)
sh["B2"] = "Opening balance"
sh["C2"] = 1234.56
sh["E2"] = True

# Row 3: the day BEFORE the phantom day.  openpyxl must write serial 59.
sh["A3"] = datetime.datetime(1900, 2, 28)
sh["B3"] = "Opening balance"       # repeated: the pool is genuinely shared
sh["C3"] = -42

# Row 4: the day AFTER it.  openpyxl must write serial 61, skipping 60.
sh["A4"] = datetime.datetime(1900, 3, 1)
sh["B4"] = "Coffee & cake <deluxe>"   # entities must survive the round trip
sh["C4"] = 0

# Row 5 is ABSENT ENTIRELY.  Row 6 has a hole at B.
sh["A6"] = datetime.datetime(1900, 1, 1)
sh["C6"] = 99

s2 = wb.create_sheet("Summary")
s2["A1"] = "Total"
s2["B1"] = 1291.56
wb.save(F + "/inline.xlsx")

# ── the 1904-system workbook ────────────────────────────────────────────
# The SAME serial means a different day here, which is the sharpest possible
# statement of trap 2: 59 is 1900-02-28 above and 1904-02-29 here, and
# 1904-02-29 is a REAL leap day.
wb2 = Workbook()
wb2.epoch = CALENDAR_MAC_1904
m = wb2.active
m.title = "Mac"
m["A1"] = datetime.datetime(1904, 1, 1)
m["A2"] = datetime.datetime(1904, 2, 29)
m["A3"] = datetime.datetime(2026, 9, 23)
wb2.save(F + "/mac1904.xlsx")

# ── re-encode the inline workbook onto a SHARED-STRING POOL ─────────────
#
# openpyxl writes `t="inlineStr"` for every text cell and no sharedStrings
# part at all -- asserted below rather than assumed.  That is the EASY case;
# Excel uses the pool, so the pool has to be tested, so it is built here and
# then verified with openpyxl's reader.
CELL = re.compile(
    rb'<c([^>]*)\st="inlineStr"([^>]*)>\s*<is>\s*<t[^>]*>(.*?)</t>\s*</is>\s*</c>', re.S)

zin = zipfile.ZipFile(F + "/inline.xlsx")
parts = {n: zin.read(n) for n in zin.namelist()}
zin.close()

assert "xl/sharedStrings.xml" not in parts, \
    "openpyxl produced a shared-string part; the premise below needs re-reading"
assert b't="inlineStr"' in parts["xl/worksheets/sheet1.xml"], \
    "openpyxl did not write inline strings; the premise below needs re-reading"

texts = set()
for name, data in parts.items():
    if name.startswith("xl/worksheets/"):
        texts.update(m.group(3) for m in CELL.finditer(data))
pool = sorted(texts)                    # sorted, so index != reading position
index = {t: i for i, t in enumerate(pool)}

for name in list(parts):
    if not name.startswith("xl/worksheets/"):
        continue
    parts[name] = CELL.sub(
        lambda m: b'<c' + m.group(1) + m.group(2) + b' t="s"><v>'
                  + str(index[m.group(3)]).encode() + b'</v></c>',
        parts[name])

sst = (b'<sst xmlns="http://schemas.openxmlformats.org/spreadsheetml/2006/main" '
       b'count="' + str(len(pool)).encode() + b'" uniqueCount="'
       + str(len(pool)).encode() + b'">'
       + b''.join(b'<si><t xml:space="preserve">' + t + b'</t></si>' for t in pool)
       + b'</sst>')
parts["xl/sharedStrings.xml"] = sst
parts["[Content_Types].xml"] = parts["[Content_Types].xml"].replace(
    b"</Types>",
    b'<Override PartName="/xl/sharedStrings.xml" ContentType="application/vnd.'
    b'openxmlformats-officedocument.spreadsheetml.sharedStrings+xml"/></Types>')
parts["xl/_rels/workbook.xml.rels"] = parts["xl/_rels/workbook.xml.rels"].replace(
    b"</Relationships>",
    b'<Relationship Id="rIdSST" Type="http://schemas.openxmlformats.org/'
    b'officeDocument/2006/relationships/sharedStrings" Target="sharedStrings.xml"/>'
    b'</Relationships>')

zout = zipfile.ZipFile(F + "/shared.xlsx", "w", zipfile.ZIP_DEFLATED)
for name, data in parts.items():
    zout.writestr(name, data)
zout.close()

# ── the re-encoding is CHECKED, not trusted ─────────────────────────────
# openpyxl's reader is independent of both this script's writer and of
# std.xlsx.  If it does not read the same values out of the pooled workbook
# as out of the inline one, the fixture is wrong and nothing below means
# anything.
a = openpyxl.load_workbook(F + "/inline.xlsx")
b = openpyxl.load_workbook(F + "/shared.xlsx")
assert a.sheetnames == b.sheetnames, (a.sheetnames, b.sheetnames)
for nm in a.sheetnames:
    for r in range(1, 8):
        for c in range(1, 6):
            av, bv = a[nm].cell(r, c).value, b[nm].cell(r, c).value
            assert av == bv, (nm, r, c, av, bv)

# ── a formula cell with a CACHED value, which openpyxl will not write ───
# openpyxl writes either the formula or the value, never both, so the cached
# form is produced here.  Scope discipline says the cached value is what a
# consumer gets; this is the fixture that says so.
parts["xl/worksheets/sheet2.xml"] = parts["xl/worksheets/sheet2.xml"].replace(
    b"</sheetData>",
    b'<row r="2"><c r="A2" t="s"><v>' + str(index[b"Total"]).encode() + b'</v></c>'
    b'<c r="B2"><f>SUM(B1:B1)</f><v>1291.56</v></c></row></sheetData>')
zout = zipfile.ZipFile(F + "/formula.xlsx", "w", zipfile.ZIP_DEFLATED)
for name, data in parts.items():
    zout.writestr(name, data)
zout.close()

# ── things that must be REFUSED ─────────────────────────────────────────
open(F + "/notazip.xlsx", "wb").write(b"this is not a zip file at all\n" * 8)
z = zipfile.ZipFile(F + "/notaworkbook.xlsx", "w", zipfile.ZIP_DEFLATED)
z.writestr("readme.txt", "a perfectly good zip that is not a workbook")
z.close()

# ── what the assertions are compared against ────────────────────────────
with open(F + "/../ref.sh", "w") as r:
    r.write("POOL_DATE='%d'\n"   % index[b"Date"])
    r.write("POOL_DESC='%d'\n"   % index[b"Description"])
    r.write("POOL_AMOUNT='%d'\n" % index[b"Amount"])
    r.write("POOL_FLAG='%d'\n"   % index[b"Flag"])
    r.write("POOL_OPEN='%d'\n"   % index[b"Opening balance"])
    r.write("POOL_COFFEE='%d'\n" % index[b"Coffee &amp; cake &lt;deluxe&gt;"])
    r.write("POOL_N='%d'\n"      % len(pool))
    # The serials openpyxl actually wrote, pulled back out of the file.
    s1 = parts["xl/worksheets/sheet1.xml"].decode()
    for label, ref in (("A2", "A2"), ("A3", "A3"), ("A4", "A4"), ("A6", "A6")):
        m = re.search(r'<c r="%s"[^>]*>\s*<v>([^<]*)</v>' % ref, s1)
        r.write("SERIAL_%s='%s'\n" % (label, m.group(1)))
    z4 = zipfile.ZipFile(F + "/mac1904.xlsx")
    s4 = z4.read("xl/worksheets/sheet1.xml").decode()
    m = re.search(r'<c r="A2"[^>]*>\s*<v>([^<]*)</v>', s4)
    r.write("SERIAL_MAC_A2='%s'\n" % m.group(1))
    r.write("DATE1904_DECLARED='%s'\n"
            % ("yes" if b'date1904="1"' in z4.read("xl/workbook.xml") else "no"))
print("fixtures ok; pool =", pool)
PY
then
    gen_died "the fixture generator exited non-zero"
fi

[ -s "${WORK}/ref.sh" ] || gen_died "no reference file was written"
for _n in POOL_DATE POOL_AMOUNT POOL_N SERIAL_A3 SERIAL_A4 SERIAL_MAC_A2 \
          DATE1904_DECLARED; do
    grep -q "^${_n}=" "${WORK}/ref.sh" || gen_died "reference file does not set ${_n}"
done
# shellcheck disable=SC1091
. "${WORK}/ref.sh" || gen_died "the reference file could not be sourced"

for _f in inline.xlsx shared.xlsx mac1904.xlsx formula.xlsx notazip.xlsx \
          notaworkbook.xlsx; do
    [ -s "${FIX}/${_f}" ] || gen_died "fixture ${_f} was not written"
done

# ════════════════════════════════════════════════════════════════════════
# 1. THE PREMISE, ASSERTED AS FACT AND NOT ASSUMED.
#
# The whole of trap 2 rests on what the ENCODER wrote, so read it back out
# of the file rather than restating the rule from memory. If openpyxl ever
# stops writing 59 and 61 for these two days, the leap-year assertions below
# are decorative and this says so first.
# ════════════════════════════════════════════════════════════════════════
expect "premise-serial-before-phantom-is-59" "${SERIAL_A3}" "59"
expect "premise-serial-after-phantom-is-61"  "${SERIAL_A4}" "61"
expect "premise-phantom-day-is-skipped" \
    "$([ $((SERIAL_A4 - SERIAL_A3)) = 2 ] && echo yes || echo no)" "yes"
expect "premise-1904-workbook-declares-it"   "${DATE1904_DECLARED}" "yes"
expect "premise-1904-real-leap-day-is-59"    "${SERIAL_MAC_A2}" "59"
# The pooled fixture's indices are NOT in reading order, so returning the
# index instead of the text cannot accidentally look right.
expect "premise-pool-index-is-not-position" \
    "$([ "${POOL_DATE}" != 0 ] && [ "${POOL_AMOUNT}" = 0 ] && echo yes || echo no)" "yes"

# ════════════════════════════════════════════════════════════════════════
# 2. LINK. A program importing std.xlsx AND NOTHING ELSE must build.
#    136.33 registered six modules' glue under the wrong name; that defect is
#    invisible unless the module is the sole import, because any other import
#    drags the glue in. std.xlsx also needs std.zip pulled in transitively,
#    which this is the only case that proves.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/link.tk" <<'TKEOF'
m=xlsxlink;
i=x:std.xlsx;

f=main():i64{
  let r=x.openfile("/nonexistent/none.xlsx");
  mt r { $ok:w x.close(w); $err:e 0 };
  <0
};
TKEOF
if "${TKC}" --out "${WORK}/link" "${WORK}/link.tk" > "${WORK}/link.log" 2>&1; then
    expect "link-xlsx-alone" "ok" "ok"
else
    fail_only "link-xlsx-alone" "a program importing only std.xlsx did not build"
    sed 's/^/        /' "${WORK}/link.log"
fi

# ════════════════════════════════════════════════════════════════════════
# 3-7. The consumer.
# ════════════════════════════════════════════════════════════════════════
cat > "${WORK}/xl.tk" <<'TKEOF'
m=xlsxconsumer;
i=io:std.io;
i=s:std.str;
i=env:std.env;
i=x:std.xlsx;

f=fix(name:str):str{
  <s.concat(s.concat(env.getor("XLFIX";"/tmp/xlfix");"/");name)
};

(* ref|row|col|celltype|raw|value -- EVERY declared field of $xlsxcell.
   A .tki/glue slot mismatch still compiles and then reads whatever sits at
   the offset (127.86), so every field is printed and compared.
   .ref, .row and .col are here for trap 3 as well: a parser that counted
   cells left to right would report the right VALUE at the wrong REFERENCE,
   and this line shows both. *)
f=cellline(c:$xlsxcell):str{
  <s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(
    c.ref;"|");s.fromint(c.row));"|");s.fromint(c.col));"|");c.celltype);"|");c.raw);"|");c.value)
};

f=dumpcell(tag:str;rows:@(@($xlsxcell));r:i64;c:i64):i64{
  if(r<rows.len){
    let row=rows.get(r);
    if(c<row.len){
      io.println(s.concat(s.concat(tag;"=");cellline(row.get(c))))
    }el{
      io.println(s.concat(tag;"=NOCOL"))
    }
  }el{
    io.println(s.concat(tag;"=NOROW"))
  };
  <0
};

f=dumpsheet(prefix:str;file:str;sheet:str):i64{
  let w=x.openfile(fix(file));
  mt w {
    $ok:b dumpbook(prefix;b;sheet);
    $err:e io.println(s.concat(s.concat(prefix;"-open=ERR|");x.lasterr()))
  };
  <0
};

f=dumpbook(prefix:str;b:$xlsxbook;sheet:str):i64{
  io.println(s.concat(s.concat(prefix;"-open=ok|");x.datesystem(b)));
  let names=x.sheets(b);
  let joined=mut."";
  lp(let i=0;i<names.len;i=i+1){
    if(i>0){ joined=s.concat(joined;",") };
    joined=s.concat(joined;names.get(i))
  };
  io.println(s.concat(s.concat(prefix;"-sheets=");joined));
  let r=x.rows(b;sheet);
  mt r {
    $ok:rows dumprows(prefix;rows);
    $err:e io.println(s.concat(s.concat(prefix;"-rows=ERR|");x.lasterr()))
  };
  x.close(b);
  <0
};

f=dumprows(prefix:str;rows:@(@($xlsxcell))):i64{
  let w=mut.0;
  if(rows.len>0){ w=rows.get(0).len };
  io.println(s.concat(s.concat(s.concat(s.concat(prefix;"-dims=");s.fromint(rows.len));"x");s.fromint(w)));
  lp(let r=0;r<rows.len;r=r+1){
    let row=rows.get(r);
    lp(let c=0;c<row.len;c=c+1){
      io.println(s.concat(s.concat(s.concat(s.concat(s.concat(s.concat(
        prefix;"-r");s.fromint(r));"c");s.fromint(c));"=");cellline(row.get(c))))
    }
  };
  <0
};

(* A workbook that must be REFUSED, and the reason the reader gave.
   Prints ACCEPTED when it opened, which is the failure. *)
f=mustreject(label:str;file:str):i64{
  let w=x.openfile(fix(file));
  mt w {
    $ok:b io.println(s.concat(s.concat("reject-";label);"=ACCEPTED"));
    $err:e io.println(s.concat(s.concat(s.concat("reject-";label);"=");x.lasterr()))
  };
  <0
};

f=badsheet():i64{
  let w=x.openfile(fix("shared.xlsx"));
  mt w {
    $ok:b badsheetbody(b);
    $err:e io.println("badsheet=OPENERR")
  };
  <0
};

f=badsheetbody(b:$xlsxbook):i64{
  let r=x.rows(b;"NoSuchSheet");
  mt r {
    $ok:rows io.println(s.concat("badsheet=ACCEPTED|";s.fromint(rows.len)));
    $err:e io.println(s.concat("badsheet=";x.lasterr()))
  };
  x.close(b);
  <0
};

(* The date mapping on its own, over the days where the 1900 bug bites.
   A fixture cannot carry serial 60: no date library has 1900-02-29. *)
f=serials(system:str):i64{
  let v=mut."";
  lp(let n=58;n<63;n=n+1){
    if(n>58){ v=s.concat(v;",") };
    v=s.concat(v;x.datefromserial(s.fromint(n);system))
  };
  io.println(s.concat(s.concat(s.concat("serials-";system);"=");v));
  <0
};

f=main():i64{
  dumpsheet("sh";"shared.xlsx";"Ledger");
  dumpsheet("in";"inline.xlsx";"Ledger");
  dumpsheet("sum";"shared.xlsx";"Summary");
  dumpsheet("mac";"mac1904.xlsx";"Mac");
  dumpsheet("fml";"formula.xlsx";"Summary");
  serials("1900");
  serials("1904");
  io.println(s.concat("serial60-1900=";x.datefromserial("60";"1900")));
  io.println(s.concat("serialfrac=";x.datefromserial("59.25";"1900")));
  io.println(s.concat("serialjunk=";x.datefromserial("not-a-number";"1900")));
  mustreject("notazip";"notazip.xlsx");
  mustreject("notaworkbook";"notaworkbook.xlsx");
  mustreject("missing";"nosuchfile.xlsx");
  badsheet();
  <0
};
TKEOF

if ! "${TKC}" --out "${WORK}/xl" "${WORK}/xl.tk" > "${WORK}/build.log" 2>&1; then
    fail_only "build-consumer" "the std.xlsx consumer did not compile"
    sed 's/^/        /' "${WORK}/build.log"
    echo "--------------------------------------"
    echo "Results: ${PASS} passed, ${FAIL} failed"
    exit 1
fi
expect "build-consumer" "ok" "ok"

OUT="$(XLFIX="${FIX}" "${WORK}/xl" --allow-read 2>&1)"

# ── 3. the workbook opens, and the sheets are named and ordered ─────────
expect "open-1900-system"  "$(line "$OUT" sh-open)"   "ok|1900"
expect "sheets-in-tab-order" "$(line "$OUT" sh-sheets)" "Ledger,Summary"
# The rectangle is 6 rows by 5 columns even though row 5 is absent from the
# file and column D appears in no row at all.
expect "rectangle-covers-the-gaps" "$(line "$OUT" sh-dims)" "6x5"

# ── 4. TRAP 1: shared strings ───────────────────────────────────────────
# `.raw` is the INDEX and `.value` is the text. Asserting both is what proves
# the resolution happened: a parser that returned the index as the value
# would match on .raw and fail on .value, and one that somehow had the text
# inline would fail on .raw.
expect "shared-A1-header"  "$(line "$OUT" sh-r0c0)" "A1|1|1|str|${POOL_DATE}|Date"
expect "shared-B1-header"  "$(line "$OUT" sh-r0c1)" "B1|1|2|str|${POOL_DESC}|Description"
expect "shared-C1-header"  "$(line "$OUT" sh-r0c2)" "C1|1|3|str|${POOL_AMOUNT}|Amount"
expect "shared-E1-header"  "$(line "$OUT" sh-r0c4)" "E1|1|5|str|${POOL_FLAG}|Flag"
# The same text used twice is ONE pool entry, reached from two cells.
expect "shared-B2-repeated" "$(line "$OUT" sh-r1c1)" "B2|2|2|str|${POOL_OPEN}|Opening balance"
expect "shared-B3-repeated" "$(line "$OUT" sh-r2c1)" "B3|3|2|str|${POOL_OPEN}|Opening balance"
# Entities survive: the pool holds `Coffee &amp; cake &lt;deluxe&gt;`.
expect "shared-entities-decoded" "$(line "$OUT" sh-r3c1)" \
    "B4|4|2|str|${POOL_COFFEE}|Coffee & cake <deluxe>"
# The inline-string workbook holds the SAME values by the other encoding, so
# both halves of trap 1 are exercised and neither is assumed.
expect "inline-A1-header"  "$(line "$OUT" in-r0c0)" "A1|1|1|str|Date|Date"
expect "inline-B4-entities" "$(line "$OUT" in-r3c1)" \
    "B4|4|2|str|Coffee & cake <deluxe>|Coffee & cake <deluxe>"

# ── 5. TRAP 2: dates are serials, and the 1900 bug is reproduced ────────
# `.raw` keeps the serial, `.value` is the resolved date. Both are asserted:
# the serial proves which number was read and the date proves how.
expect "date-modern"        "$(line "$OUT" sh-r1c0)" "A2|2|1|date|${SERIAL_A2}|2026-09-23"
# THE ONE THAT MATTERS. Serial 59 is 1900-02-28. An implementation that
# "fixed" the leap-year bug — one epoch throughout — answers 1900-02-27 here,
# and answers it silently.
expect "date-before-phantom" "$(line "$OUT" sh-r2c0)" "A3|3|1|date|59|1900-02-28"
expect "date-after-phantom"  "$(line "$OUT" sh-r3c0)" "A4|4|1|date|61|1900-03-01"
expect "date-epoch-start"    "$(line "$OUT" sh-r5c0)" "A6|6|1|date|1|1900-01-01"
# The phantom day itself, which no fixture can contain because no date
# library has 1900-02-29.
expect "serial-60-is-the-phantom-day" "$(line "$OUT" serial60-1900)" "1900-02-29"
expect "serials-around-the-bug" "$(line "$OUT" serials-1900)" \
    "1900-02-27,1900-02-28,1900-02-29,1900-03-01,1900-03-02"
# The SAME serials in the 1904 system, where the bug does NOT apply: 1904 was
# a real leap year, so 1904-02-29 is a real day and nothing is skipped.
expect "serials-1904-has-no-phantom" "$(line "$OUT" serials-1904)" \
    "1904-02-28,1904-02-29,1904-03-01,1904-03-02,1904-03-03"
# And read through a real 1904 workbook: serial 59 is a DIFFERENT DAY from
# the serial 59 in the 1900 workbook above.
expect "mac-date-system"      "$(line "$OUT" mac-open)"  "ok|1904"
expect "mac-epoch-is-serial-0" "$(line "$OUT" mac-r0c0)" "A1|1|1|date|0|1904-01-01"
expect "mac-real-leap-day"     "$(line "$OUT" mac-r1c0)" "A2|2|1|date|59|1904-02-29"
expect "mac-modern-date"       "$(line "$OUT" mac-r2c0)" "A3|3|1|date|44826|2026-09-23"
# A fractional serial carries a time of day; a non-number is not a date.
expect "serial-fraction-is-a-time" "$(line "$OUT" serialfrac)" "1900-02-28T06:00:00"
expect "serial-junk-is-empty"      "$(line "$OUT" serialjunk)" ""

# ── 6. TRAP 3: sparse rows and columns ──────────────────────────────────
# Row 2 in the file is A, B, C, E. A parser that counted cells left to right
# puts E2's `true` at column D — so D2 being EMPTY and E2 being the boolean
# is the assertion that catches it, and `.ref`/`.col` show it twice over.
expect "gap-D1-is-empty"  "$(line "$OUT" sh-r0c3)" "D1|1|4|empty||"
expect "gap-D2-is-empty"  "$(line "$OUT" sh-r1c3)" "D2|2|4|empty||"
expect "gap-E2-not-shifted" "$(line "$OUT" sh-r1c4)" "E2|2|5|bool|1|true"
expect "gap-C2-amount"    "$(line "$OUT" sh-r1c2)" "C2|2|3|num|1234.56|1234.56"
# Row 3 stops at C: D and E are both holes, and the negative number is intact.
expect "gap-C3-negative"  "$(line "$OUT" sh-r2c2)" "C3|3|3|num|-42|-42"
expect "gap-E3-is-empty"  "$(line "$OUT" sh-r2c4)" "E3|3|5|empty||"
# Zero is a VALUE, not a hole. A reader treating 0 as absent loses it.
expect "zero-is-a-value"  "$(line "$OUT" sh-r3c2)" "C4|4|3|num|0|0"
# Row 5 does not appear in the file at all and is still a full row here.
expect "missing-row-is-present-and-empty-A" "$(line "$OUT" sh-r4c0)" "A5|5|1|empty||"
expect "missing-row-is-present-and-empty-C" "$(line "$OUT" sh-r4c2)" "C5|5|3|empty||"
# Row 6 is A and C: the INTERIOR hole. A left-to-right parser puts 99 at B6.
expect "interior-gap-B6-is-empty" "$(line "$OUT" sh-r5c1)" "B6|6|2|empty||"
expect "interior-gap-C6-not-shifted" "$(line "$OUT" sh-r5c2)" "C6|6|3|num|99|99"

# ── 7. sheet selection is BY NAME, not by part order ────────────────────
expect "second-sheet-by-name" "$(line "$OUT" sum-dims)"  "1x2"
expect "second-sheet-A1"      "$(line "$OUT" sum-r0c0)"  "A1|1|1|str|6|Total"
expect "second-sheet-B1"      "$(line "$OUT" sum-r0c1)"  "B1|1|2|num|1291.56|1291.56"

# ── 8. a formula's CACHED value, which is the scope line ────────────────
# Read cell values only: the consumer gets 1291.56, never "SUM(B1:B1)".
expect "formula-cached-value" "$(line "$OUT" fml-r1c1)" "B2|2|2|num|1291.56|1291.56"

# ── 9. refusals keep their own reason ───────────────────────────────────
# "It returned an error" is asserted nowhere; the rule that fired is.
expect "reject-not-a-zip" "$(line "$OUT" reject-notazip)" \
    "zip: not a zip archive, or its central directory is corrupt"
expect "reject-not-a-workbook" "$(line "$OUT" reject-notaworkbook)" \
    "xlsx: no xl/workbook.xml — not an XLSX workbook"
expect "reject-missing-file" "$(line "$OUT" reject-missing)" \
    "zip: cannot open archive file"
expect "reject-unknown-sheet" "$(line "$OUT" badsheet)" \
    "xlsx: no such sheet in this workbook"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
# Zero PASS and zero FAIL means it did not run, which is not a success.
[ "$((PASS + FAIL))" -gt 0 ] || { echo "ERROR: no assertion ran"; exit 1; }
[ "${FAIL}" -eq 0 ]
