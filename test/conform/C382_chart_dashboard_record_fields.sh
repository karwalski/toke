#!/usr/bin/env bash
# C382_chart_dashboard_record_fields.sh — std.chart's $chartspec/$dataset
# and std.dashboard's $dashboard/$dashwidget read what their names say, and
# a $chartspec literal is accepted where chart.bar's result is (story 135.30).
#
# THE DEFECT.  tkc reads `x.f` as 8-byte slot <index of f in the .tki>.
#   - chart.bar returned the C TkChartSpec {ChartType; title; labels; ...}:
#     `spec.type` read the enum as a string pointer (SIGSEGV), and
#     chart.tojson read a toke-built $chartspec as a TkChartSpec.
#   - dashboard.new returned TkDashboard *, whose slot 0 is the title
#     pointer (`d.id`), and dashboard.addchart returned the DASHBOARD, so
#     `w.id` read the title and `w.spec` the grid width as an array.
#
# THE FIX (chart.h, chart_glue.c, dashboard.h, dashboard.c,
# dashboard_glue.c).  chart.bar builds TkChartSpecRec / TkDatasetRec, the
# .tki's layout with toke arrays; chart.tojson and dashboard.addchart
# convert a $chartspec (built or literal) with chart_spec_from_rec.  A
# TkDashboard starts with TkDashboardHead {id}; addchart returns a
# TkDashWidgetRec {id; type; spec:[byte] = the chart JSON}.  All marked;
# make check-tki-layout checks them (C389).
#
# WHAT IS ASSERTED:
#   1. chart.bar: .type, .title, .labels, .datasets[0].label/.data/.color
#   2. chart.tojson of chart.bar's result carries the title and labels
#   3. a $chartspec LITERAL (type line, two datasets) through chart.tojson
#   4. dashboard.new: two dashboards read ids 1 and 2
#   5. dashboard.addchart: .id, .type, and .spec is chart.tojson's bytes
#   6. NEGATIVE CONTROL: a second chart with other labels/title reads its
#      own fields, so nothing is canned
#
# C382_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf: 0/14; both programs die with SIGSEGV
# (the chart one on `spec.type`), before their buffered output is written.
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C382_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c382_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# run NAME: build NAME.tk and run it; prints its stdout lines joined by '|',
# then " rc=<exit status>" (a crash shows as rc=139, a build failure as
# "compile failed: <first error>").
run() {
    if ! "${TKC}" --diag-text --allow-all --out "$1" "$1.tk" > "$1.log" 2>&1; then
        echo "compile failed: $(grep -m1 '^error' "$1.log")"; return
    fi
    local out rc
    out=$(./"$1" 2>"$1.err"); rc=$?
    echo "$(printf '%s' "${out}" | tr '\n' '|') rc=${rc}"
}
echo "C382: std.chart and std.dashboard record fields (135.30)"
echo "------------------------------------------------------------"
cat > ch.tk <<'TK'
m=ch;
i=io:std.io;
i=str:std.str;
i=chart:std.chart;
f=main():i64{
  let c=chart.bar(@("a";"b");@(1.5;2.5);"Sales");
  io.println(str.concat(c.type; str.concat(" "; c.title)));
  io.println(str.concat(str.fromint(c.labels.len as i64); str.concat(" "; c.labels.get(1))));
  let d=c.datasets.get(0);
  io.println(str.concat(str.fromint(c.datasets.len as i64); str.concat(" "; d.label)));
  let v:f64=d.data.get(1);
  io.println(str.concat(str.fromint(d.data.len as i64); str.concat(" "; str.fromf64(v))));
  io.println(str.concat("color="; d.color));
  io.println(mt str.frombytes(chart.tojson(c)){$ok:v v;$err:e ""});
  let lit=$chartspec{type:"line";labels:@("x";"y";"z");
    datasets:@($dataset{label:"p";data:@(1.0;2.0;3.0);color:"red"};
               $dataset{label:"q";data:@(4.0;5.0;6.0);color:"blue"});title:"Lit"};
  io.println(mt str.frombytes(chart.tojson(lit)){$ok:v v;$err:e ""});
  let o=chart.bar(@("k");@(9.0);"Other");
  io.println(str.concat(o.title; str.concat(" "; o.labels.get(0))));
  <0
};
TK
got="$(run ch)"
f() { printf '%s' "${got% rc=*}" | cut -d'|' -f"$1"; }
expect "1a chart.bar: .type .title" "$(f 1)" "bar Sales"
expect "1b .labels" "$(f 2)" "2 b"
expect "1c .datasets[0].label" "$(f 3)" "1 data"
expect "1d .datasets[0].data" "$(f 4)" "2 2.5"
expect "1e .datasets[0].color" "$(f 5)" "color="
expect "2  chart.tojson of chart.bar: type, labels, title, data" \
    "$(f 6 | grep -c '"type":"bar".*"labels":\["a","b"\].*1\.5,2\.5.*"Sales"')" "1"
expect "3  a \$chartspec literal through chart.tojson" \
    "$(f 7 | grep -c '"type":"line".*"labels":\["x","y","z"\].*"label":"p".*"red".*"label":"q".*4,5,6.*"Lit"')" "1"
expect "6  NEGATIVE CONTROL: another chart reads its own fields" "$(f 8)" "Other k"
expect "   exit status (chart)" "${got##* }" "rc=0"

cat > da.tk <<'TK'
m=da;
i=io:std.io;
i=str:std.str;
i=chart:std.chart;
i=dashboard:std.dashboard;
f=main():i64{
  let d=dashboard.new("M";2);
  let e=dashboard.new("N";1);
  io.println(str.concat(str.fromint(d.id as i64); str.concat(" "; str.fromint(e.id as i64))));
  let c=chart.bar(@("a";"b");@(1.5;2.5);"Sales");
  let w=dashboard.addchart(d;"w1";c);
  io.println(str.concat(w.id; str.concat(" "; w.type)));
  io.println(mt str.frombytes(w.spec){$ok:v v;$err:e ""});
  io.println(mt str.frombytes(chart.tojson(c)){$ok:v v;$err:e ""});
  <0
};
TK
got="$(run da)"
expect "4  dashboard.new: ids 1 and 2" "$(f 1)" "1 2"
expect "5a dashboard.addchart: .id .type" "$(f 2)" "w1 chart"
expect "5b .spec is chart.tojson's bytes (and not empty)" \
    "$([ -n "$(f 3)" ] && [ "$(f 3)" = "$(f 4)" ] && echo same)" "same"
expect "5c ... and is the chart" "$(f 3 | grep -c '"Sales"')" "1"
expect "   exit status (dashboard)" "${got##* }" "rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
