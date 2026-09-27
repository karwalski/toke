#!/usr/bin/env bash
# C072_canvas_dashboard_new_signatures.sh — canvas.new and dashboard.new take
# the arguments their documentation shows, and use them.
#
# Story: 136.70 (the 124.2f-b/c interface-drift class, in arity).
#
# THE DEFECT.  Three layers disagreed about each constructor:
#
#   canvas.new     docs (id; w; h)   .tki (str, u32)   glue (w, h), id
#                  hard-coded "canvas"                 core canvas_new(id, w, h)
#   dashboard.new  docs (title; cols) .tki (str)       glue (title), cols
#                  hard-coded 12                       core dashboard_new(title, cols)
#
# and the same drift one call later in each documented example:
# canvas.fillrect / filltext dropped the colour and font, and
# dashboard.addchart dropped the widget id; dashboard.serve(d; port) is the
# real signature (T007 pins it) and the doc now shows the port.
#
# so the documented call of each was an E4026 arity error and `make
# check-docs` was red on both.  The public signature now follows the docs and
# the C core, and the .tki and the glue agree with it.
#
# WHY VALUES.  A build that links proves the arity only.  The canvas case
# asserts that the id, width and height reach the output — the old wrapper
# would have printed id "canvas" whatever the caller passed.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_ctornew_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C072: canvas.new / dashboard.new signatures agree with their docs"
echo "-----------------------------------------------------------------"

runs() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "p.tk"
    local out rc=0
    rm -f p_bin
    if ! "${TKC}" --allow-all --out p_bin "p.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        grep -o '"message":"[^"]*"' compile.log | head -1 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return
    fi
    out="$(./p_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${expected}', got '${out}' (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

# refuses NAME SOURCE — --check must report E4026 (the old spellings).
refuses() {
    local name="$1" src="$2"
    printf '%s\n' "${src}" > "r.tk"
    local out
    out="$("${TKC}" --check "r.tk" 2>&1)"
    if printf '%s' "${out}" | grep -q '"error_code":"E4026"'; then
        echo "  PASS: ${name} — E4026"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected E4026"
        printf '%s\n' "${out}" | head -1 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}

# A1 — the docs/stdlib/canvas.md call; the id and the size reach the HTML.
runs "canvas.new(\"mycanvas\";400;300) — id, width and height are used" \
'<canvas id="mycanvas" width="400" height="300"></canvas>' \
'm=t;
i=io:std.io;
i=s:std.str;
i=canvas:std.canvas;
f=main():i64{
  let c=canvas.new("mycanvas";400;300);
  let h=canvas.tohtml(c);
  let parts=s.split(h;"\n");
  io.println(parts.get(0));
  <0
};'

# A2 — the docs/stdlib/dashboard.md call builds and runs.
runs "dashboard.new(\"Metrics\";2) builds and runs" "ok" \
'm=t;
i=io:std.io;
i=dashboard:std.dashboard;
f=main():i64{
  let d=dashboard.new("Metrics";2);
  io.println("ok");
  <0
};'

# A3 — fillrect's colour and filltext's font and colour reach the output.
# The wrappers passed NULL for all three, so the documented six-argument
# calls were arity errors and the colour could not be set at all.
runs "fillrect colour and filltext font/colour reach the drawing ops" \
"_c.fillStyle='#3366cc'; _c.fillRect(1,2,3,4);
_c.font='24px serif'; _c.fillStyle='#ff0000'; _c.fillText('Hi',5,6);" \
'm=t;
i=io:std.io;
i=s:std.str;
i=canvas:std.canvas;
f=main():i64{
  let c=canvas.new("k";10;10);
  let c2=canvas.fillrect(c;1.0;2.0;3.0;4.0;"#3366cc");
  let c3=canvas.filltext(c2;"Hi";5.0;6.0;"24px serif";"#ff0000");
  let parts=s.split(canvas.tohtml(c3);"\n");
  io.println(parts.get(3));
  io.println(parts.get(4));
  <0
};'

# A4 — dashboard.addchart(d; id; spec) and serve(d; port) type-check (the
# serve is not run: it would bind a port).
printf '%s\n' 'm=t;
i=dashboard:std.dashboard;
i=chart:std.chart;
f=main():i64{
  let d=dashboard.new("Metrics";2);
  let c=chart.bar(@("Mon";"Tue");@(10.0;20.0);"Hits");
  let w=dashboard.addchart(d;"hits";c);
  let r=dashboard.serve(d;8080);
  <0
};' > ds.tk
if "${TKC}" --allow-all --out ds_bin ds.tk >ds.log 2>&1 && [ -x ds_bin ]; then
    echo "  PASS: dashboard.addchart(d;id;spec) and serve(d;port) build"
    PASS=$((PASS + 1))
else
    echo "  FAIL: dashboard.addchart(d;id;spec) / serve(d;port) did not build"
    grep -o '"message":"[^"]*"' ds.log | head -1 | sed 's/^/      /'
    FAIL=$((FAIL + 1))
fi

# B — the two shapes the old wrappers took are now arity errors, not calls
# that silently drop the documented argument.
refuses "canvas.new(400;300) — the old two-int shape" \
'm=t;
i=canvas:std.canvas;
f=main():i64{
  let c=canvas.new(400;300);
  <0
};'

refuses "dashboard.new(\"Metrics\") — the old title-only shape" \
'm=t;
i=dashboard:std.dashboard;
f=main():i64{
  let d=dashboard.new("Metrics");
  <0
};'

echo "-----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
