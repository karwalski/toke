#!/usr/bin/env bash
# C2170_opaque_std_fields_unreadable.sh — a field of an `"opaque": true` std
# type is refused at --check (story 138.65).
#
# THE DEFECT.  csv.tki, json.tki, dataframe.tki, router.tki, infer.tki (and
# until this story html, dashboard, llm, canvas and ml) declare fields on
# types marked `"opaque": true`, and nothing read the flag, so a field read
# compiled to a load at slot <index> of whatever the value names:
#   - `w.sep` on csv.writer(44) printed 0 (the TkCsvWriter is not that
#     record);
#   - `d.ncols` on a frame printed the column array's address and `d.nrows`
#     the column count (TkDataFrame is {cols, ncols, nrows});
#   - `r.id` on router.new() printed the route list's head pointer;
#   - `r.pos` on a $csvreader -- a registry token since 138.63 -- SIGSEGV.
#
# THE FIX.  `"opaque": true` now means what it says: no field is readable
# from toke, and the checker answers a field read with E4034 (no fix).  A
# std record whose fields ARE its C layout -- marked `tki-record` and held by
# `make check-tki-layout`: $dashboard, $htmldoc, $llmstream, $canvasop,
# $dtreemodel -- is no longer marked opaque, and its fields read as before.
# A record literal of an opaque type still builds (the glue answers it as
# the null object, 138.63), and a type the module declares itself under the
# same name is its own.
#
# Negative control: TKC=~/tk/tmpwork/wt_toke320/tkc (v3.2.0) -- the six
# refused reads check clean (built, csv.writer(44)'s sep prints 0 and a
# reader over "a,b\n1,2\n" prints pos=8 sep=0); useonly fails there too
# (the literal reader is 138.63's).  At this story's base (5a65c2e9, after
# 138.63) the $csvreader read exits 139.
#
# Stories: 138.65
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2170_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2170: fields of opaque std types are refused at --check (138.65)"
echo "----------------------------------------------------------------------"

# refused NAME GOT -- NAME.tk fails --check with E4034, `got` GOT, no fix.
refused() {
    local name="$1" want="$2" out
    out="$("${TKC}" --check "${name}.tk" 2>&1)"
    if [ $? -eq 0 ]; then fail "${name}: --check passed"; return; fi
    if ! printf '%s\n' "${out}" | grep -q '"error_code":"E4034"'; then
        fail "${name}: no E4034: $(printf '%s' "${out}" | head -1 | cut -c1-200)"; return
    fi
    if ! printf '%s\n' "${out}" | grep -q "\"got\":\"${want}\""; then
        fail "${name}: got is not ${want}: $(printf '%s' "${out}" | head -1 | cut -c1-300)"; return
    fi
    if printf '%s\n' "${out}" | grep '"error_code":"E4034"' | grep -q '"fix"'; then
        fail "${name}: E4034 carries a fix"; return
    fi
    pass "${name}: E4034 got=${want}, no fix"
}

# runs NAME WANT -- NAME.tk builds, exits 0 and prints WANT.
runs() {
    local name="$1" want="$2" got rc
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build: $(head -1 "${name}.log" | cut -c1-200)"; return
    fi
    got="$(./"${name}" --allow-all 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "${name}"
    else fail "${name}: rc=${rc}; got:"; printf '%s\n' "${got}" | sed 's/^/      /'; fi
}

# ── refused: each opaque std type, by each route a value reaches a read ──
cat > csvreader.tk <<'EOF'
m=c;
i=io:std.io;
i=csv:std.csv;
i=str:std.str;
f=main():i64{
  let r=csv.reader(str.tobytes("a,b\n");44);
  io.println("\(r.pos)");
  <0
};
EOF
refused csvreader '$csvreader.pos'

cat > csvwriter.tk <<'EOF'
m=c;
i=io:std.io;
i=csv:std.csv;
f=sepof(w:$csvwriter):u8{<w.sep};
f=main():i64{
  io.println("\(sepof(csv.writer(44)))");
  <0
};
EOF
refused csvwriter '$csvwriter.sep'

cat > dataframe.tk <<'EOF'
m=c;
i=io:std.io;
i=df:std.dataframe;
f=main():i64{
  io.println("\(mt df.fromcsv("a,b\n1,2\n"){$ok:d d.ncols;$err:e 0})");
  <0
};
EOF
refused dataframe '$dataframe.ncols'

cat > router.tk <<'EOF'
m=c;
i=io:std.io;
i=router:std.router;
f=main():i64{
  let rs=@(router.new());
  io.println("\(rs.get(0).id)");
  <0
};
EOF
refused router '$router.id'

cat > jsonstream.tk <<'EOF'
m=c;
i=io:std.io;
i=json:std.json;
t=$box{s:$jsonstream};
f=main():i64{
  let b=$box{s:$jsonstream{buf:@();pos:3;depth:0;state:0}};
  io.println("\(b.s.depth)");
  <0
};
EOF
refused jsonstream '$jsonstream.depth'

cat > modelhandle.tk <<'EOF'
m=c;
i=io:std.io;
i=infer:std.infer;
f=main():i64{
  let h=5 as $modelhandle;
  io.println("\(h.id)");
  <0
};
EOF
refused modelhandle '$modelhandle.id'

# ── still readable: records whose fields are their C layout ──────────────
cat > readable.tk <<'EOF'
m=c;
i=io:std.io;
i=html:std.html;
i=dashboard:std.dashboard;
i=canvas:std.canvas;
i=ml:std.ml;
i=llm:std.llm;
f=main():i64{
  let h1=html.doc();
  let h2=html.doc();
  let d1=dashboard.new("a";2);
  let d2=dashboard.new("b";2);
  io.println("htmldoc=\(h2.id-h1.id) dashboard=\(d2.id-d1.id)");
  let c=canvas.new("c";10;10);
  canvas.fillrect(c;1.0;2.0;3.0;4.0;"red");
  io.println("canvasop=\(c.ops.get(0).tag)");
  let rows=@($row{vals:@(1.0)};$row{vals:@(9.0)});
  let m=mt ml.dtreefit(rows;@("lo";"hi");2){$ok:x x;$err:e $dtreemodel{nodes:0;maxdepth:0;arena:0}};
  io.println("dtreemodel=\(m.maxdepth)");
  let s=$llmstream{id:4};
  io.println("llmstream=\(s.id)");
  <0
};
EOF
runs readable 'htmldoc=1 dashboard=1
canvasop=fillRect
dtreemodel=2
llmstream=4'

# ── an opaque type's record literal and functions are unaffected ─────────
cat > useonly.tk <<'EOF'
m=c;
i=io:std.io;
i=csv:std.csv;
i=str:std.str;
i=df:std.dataframe;
f=main():i64{
  let r=csv.reader(str.tobytes("a,b\n1,2\n");44);
  io.println("next=\(mt csv.next(r){$ok:x x.fields.len;$err:e 0}) atend=\(csv.atend(r))");
  csv.close(r);
  let lit=$csvreader{data:@(97;10);pos:0;sep:44;line:0};
  io.println("literal=\(mt csv.next(lit){$ok:x "OK";$err:e e.kind})");
  let w=csv.writer(44);
  csv.writerow(w;@("a";"b"));
  io.println("flush=\(csv.flush(w).len)");
  let sh=df.shape(mt df.fromcsv("a,b\n1,2\n3,4\n5,6\n"){$ok:d d;$err:e 0 as $dataframe});
  io.println("shape=\(sh.rows)x\(sh.cols)");
  <0
};
EOF
runs useonly 'next=2 atend=false
literal=badarg
flush=3
shape=3x2'

# ── a module's own type of the same name is its own ──────────────────────
cat > owntype.tk <<'EOF'
m=c;
i=io:std.io;
i=csv:std.csv;
t=$csvwriter{sep:u8};
f=main():i64{
  let w=$csvwriter{sep:9};
  io.println("sep=\(w.sep)");
  <0
};
EOF
runs owntype 'sep=9'

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
