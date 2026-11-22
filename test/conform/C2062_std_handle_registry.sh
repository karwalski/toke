#!/usr/bin/env bash
# C2062_std_handle_registry.sh — std.vec, std.str's builder, std.securemem,
# std.pdf, std.xlsx, std.zip, std.toml and std.svg answer a handle they did not create
# as they answer the null handle, and never read through it (story 138.45,
# the T-67 class beyond std.process; owner answer T-67).
#
# THE DEFECT.  Each module handed its C object's address to the program as
# the opaque record (vechandle, strbuf, securebuf, pdfdoc, ziparchive,
# tomlval, svgelem; and svgdoc) and every wrapper cast the i64 back and read
# through it.  138.35's checker refuses an integer in those slots, but a value
# cast with `as` (and a value through C) still reached the runtime:
# `vec.len(5 as $vechandle)`, `str.done(..)`, `securemem.wipe(..)`,
# `pdf.pagecount(..)`, `zip.entries(..)`, `toml.keys(..)`,
# `svg.render(5 as $svgdoc)`, a forged element inside `svg.group`'s array --
# each died with SIGSEGV (Z213 survey).  A second pdf.close / zip.close /
# str.done freed the object twice, and a toml section read after toml.free
# read freed memory.
#
# THE FIX.  One registry of live std handles in tk_runtime.c (138.36's
# process table, lifted, with a kind per slot): every handle a module hands
# out is registered, and every wrapper looks its argument up without reading
# it.  A value the module did not hand out, one of another kind, and one
# already closed/finished/freed are answered as NULL is.  A toml section is a
# view owned by its document and is refused once the document is freed.
# The kinds with a release call (strbuf, pdfdoc, xlsxbook, ziparchive,
# tomlval) hand out a registry token, not the address, so a released handle
# stays refused after malloc gives its address to a newer object.
#
# WHAT IS ASSERTED, per module: the forged value's null answer, a real handle
# still working, and the closed/finished/freed handle refused afterwards --
# also when a new object of the same kind was made in between (*stale).
#
# Negative control: TKC=<base ee27591c tree>/toke -- every forged program
# exits 139 (or aborts on a double free); the real-handle lines pass.
#
# Story: 138.45
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 writes the zip and pdf fixtures"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2062_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2062: std modules refuse a handle they did not create (138.45)"
echo "----------------------------------------------------------------------"

python3 - <<'PY'
import zipfile
with zipfile.ZipFile('t.zip', 'w') as z: z.writestr('a.txt', 'hello')
stream = b"BT /F1 12 Tf 10 50 Td (Hello) Tj ET"
objs = [b"<< /Type /Catalog /Pages 2 0 R >>",
        b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 200 100] /Contents 4 0 R"
        b" /Resources << /Font << /F1 5 0 R >> >> >>",
        b"<< /Length %d >>\nstream\n" % len(stream) + stream + b"\nendstream",
        b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"]
out, offs = b"%PDF-1.4\n", []
for i, o in enumerate(objs):
    offs.append(len(out)); out += b"%d 0 obj\n" % (i + 1) + o + b"\nendobj\n"
x = len(out)
out += b"xref\n0 %d\n0000000000 65535 f \n" % (len(objs) + 1)
for o in offs: out += b"%010d 00000 n \n" % o
out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (len(objs) + 1, x)
open('t.pdf', 'wb').write(out)
# a one-sheet workbook (the parts xlsx.c reads; no openpyxl needed)
ns = 'http://schemas.openxmlformats.org/'
with zipfile.ZipFile('t.xlsx', 'w') as z:
    z.writestr('[Content_Types].xml', '<?xml version="1.0"?><Types xmlns="%spackage/2006/content-types">'
        '<Default Extension="rels" ContentType="application/vnd.openxmlformats-package.relationships+xml"/>'
        '<Default Extension="xml" ContentType="application/xml"/></Types>' % ns)
    z.writestr('_rels/.rels', '<?xml version="1.0"?><Relationships xmlns="%spackage/2006/relationships">'
        '<Relationship Id="rId1" Type="%sofficeDocument/2006/relationships/officeDocument" Target="xl/workbook.xml"/>'
        '</Relationships>' % (ns, ns))
    z.writestr('xl/workbook.xml', '<?xml version="1.0"?><workbook xmlns="%sspreadsheetml/2006/main" '
        'xmlns:r="%sofficeDocument/2006/relationships"><sheets><sheet name="Data" sheetId="1" r:id="rId1"/>'
        '</sheets></workbook>' % (ns, ns))
    z.writestr('xl/_rels/workbook.xml.rels', '<?xml version="1.0"?><Relationships xmlns="%spackage/2006/relationships">'
        '<Relationship Id="rId1" Type="%sofficeDocument/2006/relationships/worksheet" Target="worksheets/sheet1.xml"/>'
        '</Relationships>' % (ns, ns))
    z.writestr('xl/worksheets/sheet1.xml', '<?xml version="1.0"?><worksheet xmlns="%sspreadsheetml/2006/main">'
        '<sheetData><row r="1"><c r="A1"><v>7</v></c></row></sheetData></worksheet>' % ns)
PY

# case NAME IMPORTS BODY EXPECTED -- build and run one program
case_() {
    local name="$1" imports="$2" body="$3" want="$4"
    { echo "m=${name};"; echo "i=io:std.io;"; printf '%s\n' "${imports}";
      echo "f=main():i64{"; echo "  let n=5;"; printf '%s\n' "${body}"; echo "  <0"; echo "};"; } > "${name}.tk"
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build: $(grep -m1 -E 'error' "${name}.log" | cut -c1-200)"; return
    fi
    local got rc
    got="$(./"${name}" --allow-all 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "${name}"
    else fail "${name}: rc=${rc}; got:"; printf '%s\n' "${got}" | sed 's/^/      /'; echo "    want:"; printf '%s\n' "${want}" | sed 's/^/      /'; fi
}

case_ vecforged 'i=vec:std.vec;' '
  let v=n as $vechandle;
  let p=vec.push(v;7);
  io.println("len=\(vec.len(v)) pop=\(vec.pop(v)) arr=\(vec.toarray(v).len) push=\(vec.len(p))");
  let r=vec.push(vec.new();7);
  io.println("real len=\(vec.len(r)) get=\(vec.get(r;0)) idx=\(r.get(0))");' \
'len=0 pop=0 arr=0 push=0
real len=1 get=7 idx=7'

case_ strbufforged 'i=str:std.str;' '
  let b=n as $strbuf;
  str.add(b;"x");
  str.addbyte(b;99);
  io.println("done=[\(str.done(b))]");
  let sb=str.buf();
  str.add(sb;"ab");
  str.addbyte(sb;99);
  io.println("built=[\(str.done(sb))] again=[\(str.done(sb))]");' \
'done=[]
built=[abc] again=[]'

case_ securememforged 'i=securemem:std.securemem;' '
  let fs=n as $securebuf;
  io.println("write=\(securemem.write(fs;"x")) none=\(securemem.read(fs)=="") wipe=\(securemem.wipe(fs))");
  let sb=securemem.alloc(32;60);
  io.println("real write=\(securemem.write(sb;"secret")) same=\(securemem.read(sb)=="secret") wipe=\(securemem.wipe(sb))");' \
'write=false none=false wipe=false
real write=true same=true wipe=true'

case_ pdfforged 'i=pdf:std.pdf;' '
  let fp=n as $pdfdoc;
  io.println("count=\(pdf.pagecount(fp)) w=\(pdf.pagewidth(fp;1)) hastext=\(pdf.hastext(fp;1)) text=\(mt pdf.pagetext(fp;1){$ok:t t;$err:e "ERR"}) runs=\(mt pdf.runs(fp;1){$ok:r "OK";$err:e "ERR"})");
  pdf.close(fp);
  let d=mt pdf.openfile("t.pdf"){$ok:v v;$err:e 0 as $pdfdoc};
  io.println("real count=\(pdf.pagecount(d)) text=\(mt pdf.pagetext(d;1){$ok:t t;$err:e "ERR"})");
  pdf.close(d);
  io.println("closed count=\(pdf.pagecount(d)) text=\(mt pdf.pagetext(d;1){$ok:t t;$err:e "ERR"})");
  pdf.close(d);' \
'count=0 w=0 hastext=false text=ERR runs=ERR
real count=1 text=Hello
closed count=0 text=ERR'

case_ zipforged 'i=zip:std.zip;' '
  let fz=n as $ziparchive;
  io.println("entries=\(zip.entries(fz).len) read=\(mt zip.read(fz;"a.txt"){$ok:b "OK";$err:e "ERR"})");
  zip.close(fz);
  let z=mt zip.openfile("t.zip"){$ok:v v;$err:e 0 as $ziparchive};
  io.println("real entries=\(zip.entries(z).len) read=\(mt zip.read(z;"a.txt"){$ok:b b.len;$err:e -1})");
  zip.close(z);
  io.println("closed entries=\(zip.entries(z).len)");
  zip.close(z);' \
'entries=0 read=ERR
real entries=1 read=5
closed entries=0'

case_ tomlforged 'i=toml:std.toml;' '
  let ft=n as $tomlval;
  io.println("keys=\(toml.keys(ft).len) has=\(toml.has(ft;"a")) len=\(toml.len(ft)) i=\(mt toml.i64(ft;"a"){$ok:v v;$err:e -1}) free=\(toml.free(ft))");
  let doc=mt toml.load("a=1\n[s]\nb=2\n[[t]]\nc=3\n"){$ok:v v;$err:e 0 as $tomlval};
  let sec=mt toml.section(doc;"s"){$ok:v v;$err:e 0 as $tomlval};
  let arr=mt toml.sectionarr(doc;"t"){$ok:v v;$err:e @()};
  let t0=arr.get(0);
  io.println("real a=\(mt toml.i64(doc;"a"){$ok:v v;$err:e -1}) b=\(mt toml.i64(sec;"b"){$ok:v v;$err:e -1}) c=\(mt toml.i64(t0;"c"){$ok:v v;$err:e -1})");
  io.println("free view=\(toml.free(sec)) doc=\(toml.free(doc)) again=\(toml.free(doc))");
  io.println("after free b=\(mt toml.i64(sec;"b"){$ok:v v;$err:e -1}) c=\(mt toml.i64(t0;"c"){$ok:v v;$err:e -1}) len=\(toml.len(doc))");' \
'keys=0 has=false len=0 i=-1 free=false
real a=1 b=2 c=3
free view=false doc=true again=false
after free b=-1 c=-1 len=0'

case_ svgforged 'i=svg:std.svg;' '
  let sd=svg.doc(10.0;10.0);
  let fe=n as $svgelem;
  let sd2=svg.append(sd;fe);
  let st=svg.style("red";"blue";1.0);
  let sd3=svg.append(sd2;svg.rect(1.0;1.0;2.0;2.0;st));
  let ia=@(5;7);
  let ea=ia as @($svgelem);
  let g=svg.group(ea;"");
  let sd4=svg.append(sd3;g);
  let fd=n as $svgdoc;
  let fd2=svg.append(fd;svg.rect(1.0;1.0;2.0;2.0;st));
  io.println("elems=\(sd4.elements.len) out=\(svg.render(sd4).len>0) forgeddoc=[\(svg.render(fd2))]");' \
'elems=2 out=true forgeddoc=[]'

case_ xlsxforged 'i=xlsx:std.xlsx;' '
  let fx=n as $xlsxbook;
  io.println("sheets=\(xlsx.sheets(fx).len) ds=[\(xlsx.datesystem(fx))] rows=\(mt xlsx.rows(fx;"Data"){$ok:r "OK";$err:e "ERR"})");
  xlsx.close(fx);
  xlsx.close(fx);
  let w=mt xlsx.openfile("t.xlsx"){$ok:v v;$err:e 0 as $xlsxbook};
  io.println("real sheets=\(xlsx.sheets(w).len) rows=\(mt xlsx.rows(w;"Data"){$ok:r r.len;$err:e -1})");
  xlsx.close(w);
  io.println("closed sheets=\(xlsx.sheets(w).len) rows=\(mt xlsx.rows(w;"Data"){$ok:r r.len;$err:e -1})");
  xlsx.close(w);' \
'sheets=0 ds=[1900] rows=ERR
real sheets=1 rows=1
closed sheets=0 rows=-1'

# STALE HANDLES AFTER REUSE (138.45 review).  A released handle stays refused
# when a NEW object of the same kind is made between the release and the
# stale use -- the common case, where malloc hands the freed address to the
# new object.  Keyed by address, the stale handle acted on the new object
# (b2=twoSTALE; toml.i64(doc1) answered the new document's 2).
case_ strbufstale 'i=str:std.str;' '
  let b1=str.buf();
  str.add(b1;"one");
  io.println("b1=[\(str.done(b1))]");
  let b2=str.buf();
  str.add(b2;"two");
  str.add(b1;"STALE");
  str.addbyte(b1;33);
  io.println("stale done=[\(str.done(b1))] b2=[\(str.done(b2))]");' \
'b1=[one]
stale done=[] b2=[two]'

case_ tomlstale 'i=toml:std.toml;' '
  let d1=mt toml.load("a=1\n[s]\nb=10\n"){$ok:v v;$err:e 0 as $tomlval};
  let s1=mt toml.section(d1;"s"){$ok:v v;$err:e 0 as $tomlval};
  io.println("freed=\(toml.free(d1))");
  let d2=mt toml.load("a=2\n[s]\nb=20\n"){$ok:v v;$err:e 0 as $tomlval};
  let s2=mt toml.section(d2;"s"){$ok:v v;$err:e 0 as $tomlval};
  io.println("stale a=\(mt toml.i64(d1;"a"){$ok:v v;$err:e -1}) b=\(mt toml.i64(s1;"b"){$ok:v v;$err:e -1}) free=\(toml.free(d1))");
  io.println("new a=\(mt toml.i64(d2;"a"){$ok:v v;$err:e -1}) b=\(mt toml.i64(s2;"b"){$ok:v v;$err:e -1})");' \
'freed=true
stale a=-1 b=-1 free=false
new a=2 b=20'

case_ pdfstale 'i=pdf:std.pdf;' '
  let d1=mt pdf.openfile("t.pdf"){$ok:v v;$err:e 0 as $pdfdoc};
  pdf.close(d1);
  let d2=mt pdf.openfile("t.pdf"){$ok:v v;$err:e 0 as $pdfdoc};
  pdf.close(d1);
  io.println("stale count=\(pdf.pagecount(d1)) new count=\(pdf.pagecount(d2))");
  pdf.close(d2);' \
'stale count=0 new count=1'

case_ zipstale 'i=zip:std.zip;' '
  let z1=mt zip.openfile("t.zip"){$ok:v v;$err:e 0 as $ziparchive};
  zip.close(z1);
  let z2=mt zip.openfile("t.zip"){$ok:v v;$err:e 0 as $ziparchive};
  zip.close(z1);
  io.println("stale entries=\(zip.entries(z1).len) new entries=\(zip.entries(z2).len)");
  zip.close(z2);' \
'stale entries=0 new entries=1'

case_ xlsxstale 'i=xlsx:std.xlsx;' '
  let w1=mt xlsx.openfile("t.xlsx"){$ok:v v;$err:e 0 as $xlsxbook};
  xlsx.close(w1);
  let w2=mt xlsx.openfile("t.xlsx"){$ok:v v;$err:e 0 as $xlsxbook};
  xlsx.close(w1);
  io.println("stale sheets=\(xlsx.sheets(w1).len) new sheets=\(xlsx.sheets(w2).len)");
  xlsx.close(w2);' \
'stale sheets=0 new sheets=1'

echo "----------------------------------------------------------------------"
echo "C2062: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
