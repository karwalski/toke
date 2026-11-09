#!/usr/bin/env bash
# C1120_std_field_record_array.sh -- a std record field that is an array of
# std records keeps its element type (127.335).
#
# THE DEFECT (fix/z126-tki-field-inner-type 7ce76983).  imported_struct_type
# adopted a std array field only when its innermost element was
# str/bool/f64 (127.333).  A field whose elements are std records --
# ml.kmeansmodel.centroids "[centroid]", chart.chartspec.datasets
# "[dataset]", llmtool.tooldecl.params "[toolparam]", svg.svgdoc.elements
# "[svgelem]", canvas.canvas.ops "[canvasop]" -- stayed TY_UNKNOWN, so
# `km.centroids.get(0).center.get(1)` printed 2.5's bits
# (4612811918334230528), as did a dataset's data.
#
# THE FIX.  types.c imported_struct_type: a std field `[`^n NAME `]`^n whose
# NAME is itself a std record (not a sum) takes imported_struct_type(NAME)
# wrapped in n arrays; `depth` (< 4) bounds a record that names its own type.
# Each glue was checked to hand toke an array of record pointers laid out as
# the element's .tki (ml_glue.c kmeanstrain, chart_glue.c bar,
# llm_tool_glue.c decode_tools, svg.c append, canvas.c record_op).
#
# The recursion cases need a std record that names itself, and no shipped
# interface has one, so they run against a mirror of the stdlib directory
# (symlinks) with one extra type-only interface, std.zrec.
#
# Negative control: C1120_TKC=<other tkc> C1120_ROOT=<its tree>.  At 7ce76983
# k1, k2, d1, n1 and r1 fail; the c* controls pass (the tooldecl and canvas
# reads were already right through the emitter's field-owner search).
#
# Story: 127.335
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${C1120_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
TKC="${C1120_TKC:-${TKC:-${REPO_ROOT}/tkc}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1120_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
export TKC_STDLIB_DIR="${REPO_ROOT}/src/stdlib"
echo "C1120: a std field that is an array of std records keeps its element type (127.335)"
echo "---------------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# runs name description expected-output  (program after the header on stdin)
runs() {
    local name=$1 desc=$2 want=$3 got
    { printf 'm=%s;i=io:std.io;\n' "${name}"; cat; } > "${name}.tk"
    if "${TKC}" -o "${name}" "${name}.tk" >/dev/null 2>&1; then
        got="$(./"${name}" 2>/dev/null | tr '\n' ' ')"
    else got="build-failed: $("${TKC}" --check "${name}.tk" 2>&1 | head -c 300)"; fi
    if [ "${got}" = "${want}" ]; then pass "${desc} (${want})"
    else fail "${desc}: expected '${want}', got '${got}'"; fi
}

# ── [centroid] ──────────────────────────────────────────────────────────
runs k1 'kmeansmodel.centroids literal: chained, bound, through a parameter' \
     '2.5 2.5 0.25 7 ' <<'EOF'
i=ml:std.ml;
f=c0(km:$kmeansmodel):f64{<km.centroids.get(0).center.get(0)};
f=main():i64{let km=$kmeansmodel{centroids:@($centroid{id:7;center:@(0.25;2.5)});k:1};
let c=km.centroids.get(0);
io.println("\(km.centroids.get(0).center.get(1)) \(c.center.get(1)) \(c0(km)) \(c.id)");<0};
EOF
runs k2 'centroids built by the ml.kmeanstrain glue' '2 10.2 ' <<'EOF'
i=ml:std.ml;
f=main():i64{
let rs=@($row{vals:@(1.0;1.0)};$row{vals:@(1.2;1.0)};$row{vals:@(9.0;9.0)};$row{vals:@(9.2;9.0)});
let m=mt ml.kmeanstrain(rs;2;20) {$ok:v v;$err:e $kmeansmodel{centroids:@();k:0}};
let s=m.centroids.get(0).center.get(0)+m.centroids.get(1).center.get(0);
io.println("\(m.k) \(s)");<0};
EOF

# ── [dataset] (a literal, and the chart.bar glue) ───────────────────────
runs d1 'chartspec.datasets: literal and chart.bar' '1.5 red 2.5 data ' <<'EOF'
i=chart:std.chart;
f=main():i64{let ds=$dataset{label:"s";data:@(1.5;2.5);color:"red"};
let sl=$chartspec{type:"line";labels:@("a");datasets:@(ds);title:"t"};
let sp=chart.bar(@("a";"b");@(1.5;2.5);"t");
io.println("\(sl.datasets.get(0).data.get(0)) \(sl.datasets.get(0).color) \(sp.datasets.get(0).data.get(1)) \(sp.datasets.get(0).label)");<0};
EOF

# ── the field is judged as the type it now has ──────────────────────────
printf 'm=n1;i=io:std.io;i=ml:std.ml;\nf=main():i64{let km=$kmeansmodel{centroids:@($centroid{id:1;center:@(1.5)});k:1};let x:str=km.centroids.get(0).center;<0};\n' > n1.tk
out="$("${TKC}" --check n1.tk 2>&1)"; rc=$?
if [ "${rc}" -eq 1 ] && printf '%s' "${out}" | grep -q '"error_code":"E4031".*"expected":"str","got":"@(f64)"'; then
    pass "let x:str= a centroid's center is E4031 (expected str, got @(f64))"
else fail "n1: want exit 1 and E4031 str vs @(f64), got exit ${rc}: ${out}"; fi

# ── controls: right before and stay right ───────────────────────────────
runs c1 'tooldecl.params / svgdoc.elements / canvas.ops' 'p true 1 1 fillRect ' <<'EOF'
i=lt:std.llmtool;i=svg:std.svg;i=cv:std.canvas;
f=main():i64{
let td=$tooldecl{name:"f";desc:"d";params:@($toolparam{name:"p";type:"str";desc:"x";required:true})};
let st=$svgstyle{fill:"red";stroke:"none";strokewidth:1.0;opacity:1.0;fontsize:10.0;fontfamily:"x"};
let d=svg.append(svg.doc(10.0;10.0);svg.rect(0.0;0.0;1.0;1.0;st));
let c=cv.fillrect(cv.new("c";10;10);0.0;0.0;1.0;1.0;"red");
io.println("\(td.params.get(0).name) \(td.params.get(0).required) \(d.elements.len) \(c.ops.len) \(c.ops.get(0).tag)");<0};
EOF

# ── recursion: a std record naming itself, and a two-record cycle ───────
MIR="${WORK}/mir"; mkdir -p "${MIR}/src/stdlib" "${MIR}/stdlib"
for f in "${REPO_ROOT}"/src/stdlib/*; do ln -s "${f}" "${MIR}/src/stdlib/"; done
for f in "${REPO_ROOT}"/stdlib/*;     do ln -s "${f}" "${MIR}/stdlib/"; done
cat > "${MIR}/stdlib/zrec.tki" <<'EOF'
{"schema_version":"1.0","module":"std.zrec","exports":[
 {"kind":"type","name":"znode","fields":[{"name":"v","type":"[f64]"},{"name":"kids","type":"[znode]"}]},
 {"kind":"type","name":"za","fields":[{"name":"w","type":"f64"},{"name":"bs","type":"[[zb]]"}]},
 {"kind":"type","name":"zb","fields":[{"name":"s","type":"[str]"},{"name":"aa","type":"[za]"},{"name":"u","type":"[zundeclared]"}]}
]}
EOF
TKC_STDLIB_DIR="${MIR}/src/stdlib" runs r1 'self-referential [znode], two levels down' '2.5 1.5 1 ' <<'EOF'
i=z:std.zrec;
f=main():i64{let leaf=$znode{v:@(0.5;1.5);kids:@()};let mid=$znode{v:@(2.5);kids:@(leaf)};
let top=$znode{v:@(3.5);kids:@(mid)};
io.println("\(top.kids.get(0).v.get(0)) \(top.kids.get(0).kids.get(0).v.get(1)) \(top.kids.len)");<0};
EOF
TKC_STDLIB_DIR="${MIR}/src/stdlib" runs c2 'za/zb cycle through [[zb]], and an undeclared element type' 'q 1 0 ' <<'EOF'
i=z:std.zrec;
f=main():i64{let b=$zb{s:@("q");aa:@();u:@()};let a=$za{w:1.5;bs:@(@(b))};
io.println("\(a.bs.get(0).get(0).s.get(0)) \(a.bs.get(0).len) \(b.u.len)");<0};
EOF

echo "---------------------------------------------------------------------------"
echo "C1120: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
