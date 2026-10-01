#!/usr/bin/env bash
# C200_markup_output_escaping.sh — std.svg, std.canvas and std.chart escape
# every caller string for the context it is written into (story 122.13 (1)).
#
# THE DEFECTS (measured on main before 122.13).
#   svg     text content, fill/stroke/font-family, path d, transform, the
#           ellipse style, gradient ids/stop colours and animate values were
#           written raw: a label "</text><script>..." injected an element and
#           a fill 'red" onload="...' injected an attribute.
#   canvas  colour, font, text and the canvas id went into '...' JavaScript
#           literals raw ("');alert(1);//" ran code; "</script>" closed the
#           element canvas_to_html wraps them in), the image src into a
#           /* */ comment raw ("*/alert(1)/*" ran code), the id into an HTML
#           attribute raw.
#   chart   the heatmap (SVG) wrote its title and row/column labels raw;
#           chart_set_theme wrote the theme name into JSON raw ('dark","x":"1'
#           added a key); the JSON string escaper passed control bytes other
#           than \n \r \t through (invalid JSON), and NaN/Inf printed as
#           "nan"/"inf" (invalid JSON).
#
# WHAT THIS DRIVES (ASan+UBSan build of svg.c, canvas.c, chart.c):
#   every planted string is checked in the output two ways -- the injected
#   construct is absent, and the output PARSES (python3's XML and JSON
#   parsers; node --check for the canvas JavaScript when node is present,
#   a named SKIP line otherwise) with the planted text recovered intact
#   from the parse, so escaping did not corrupt it.  Ordinary strings
#   (no special characters) are compared byte-for-byte against the
#   expected markup, so escaping did not change ordinary output.
#
# Negative control: C200_STDLIB=<dir> builds against another stdlib tree.
# A copy of src/stdlib/ with main's svg.c canvas.c chart.c strbuf.h put
# back fails every injection check.
#
# Story: 122.13

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C200_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c200_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C200: svg/canvas/chart escape caller strings for their output context"
echo "---------------------------------------------------------------------"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found (needed to parse the XML/JSON output)"
    echo "CONFORM-SKIP: python3 not installed"
    exit 0
fi

cat > "${WORK}/drv.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svg.h"
#include "canvas.h"
#include "chart.h"
static char dir[1024];
static void put(const char *name, const char *s) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w"); if (!f) exit(3);
    fputs(s ? s : "(null)", f); fclose(f);
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    snprintf(dir, sizeof dir, "%s", argv[1]);
    /* ---- svg, hostile ---- */
    TkSvgDoc *d = svg_doc(100, 50);
    TkSvgStyle st = svg_style("red\" onload=\"alert(1)", "blue'<x>", 1.0);
    st.font_family = "A&B \"</text><script>alert(3)</script>";
    svg_append(d, svg_text(1, 2, "T</text><script>alert(2)</script>&amp;", st));
    svg_append(d, svg_path("M0 0\"/><script>alert(4)</script><path d=\"", svg_style("none", NULL, 0)));
    TkSvgElem *kid[1] = { svg_rect(0, 0, 1, 1, svg_style("green", NULL, 0)) };
    svg_append(d, svg_group(kid, 1, "translate(1)\" onclick=\"alert(5)"));
    svg_ellipse(d, 1, 1, 1, 1, "fill:red\"/><script>alert(6)</script><x a=\"");
    SvgStop stops[1] = { { 0.5, "red\"/><script>alert(7)</script><stop x=\"", 1.0 } };
    svg_gradient_linear(d, "g\"/><script>alert(8)</script><x y=\"", stops, 1, 0, 0, 1, 1);
    put("svg_hostile.svg", svg_render(d));
    svg_free(d);
    /* ---- svg, ordinary ---- */
    d = svg_doc(10, 10);
    svg_append(d, svg_text(1, 2, "Hello world", svg_style("#333", "none", 0.5)));
    svg_append(d, svg_path("M0 0 L10 10", svg_style("none", "black", 1)));
    put("svg_plain.svg", svg_render(d));
    svg_free(d);
    /* ---- canvas, hostile ---- */
    TkCanvas *c = canvas_new("cv\"><script>alert(1)</script><x id='", 10, 10);
    canvas_fill_text(c, "');alert(2);//</script><script>alert(3)</script>",
                     1, 2, "12px x');alert(4);//", "#000');alert(5);//");
    canvas_fill_rect(c, 0, 0, 1, 1, "red\";alert(6);//");
    canvas_draw_image(c, "a.png */alert(7);/*", 0, 0, 1, 1);
    canvas_fill_style(c, "x alert(8)");
    put("canvas_hostile.html", canvas_to_html(c));
    put("canvas_hostile.js", canvas_to_js(c));
    canvas_free(c);
    /* ---- canvas, ordinary ---- */
    c = canvas_new("chart1", 20, 10);
    canvas_fill_text(c, "Total 42", 1, 2, "12px sans-serif", "#333");
    canvas_fill_rect(c, 0, 0, 5, 5, "rgba(0,0,0,0.5)");
    canvas_draw_image(c, "logo.png", 0, 0, 4, 4);
    put("canvas_plain.js", canvas_to_js(c));
    canvas_free(c);
    /* ---- chart JSON ---- */
    const char *labels[3] = { "a\001b", "q\"x", "nl\nx" };
    StrArray la = { labels, 3 };
    double vals[3] = { 1.5, 0.0 / 0.0, 1.0 / 0.0 };
    TkDataset ds = { "s\x7f", vals, 3, NULL };
    TkChartSpec *sp = chart_bar(la, &ds, 1, "T\x02\"</script>");
    const char *j = chart_tojson(sp);
    put("chart.json", j);
    put("chart_vega.json", chart_tovega(sp));
    put("chart_theme.json", chart_set_theme(j, "dark\",\"injected\":\"1"));
    chart_free(sp);
    /* ---- chart heatmap (SVG) ---- */
    const char *rows[1] = { "r</text><script>alert(1)</script>" };
    const char *cols[2] = { "c&1", "c\"2" };
    double m[2] = { 1.0, 2.0 };
    put("heatmap.svg", chart_heatmap(rows, 1, cols, 2, m, "H</text><script>alert(2)</script>"));
    return 0;
}
C

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
LINK=()
if LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" svg canvas chart 2>"${WORK}/cc.log")"; then
    while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
fi
if [ "${#LINK[@]}" -eq 0 ] || ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" \
        -o "${WORK}/drv" "${WORK}/drv.c" "${LINK[@]}" \
        -lz -lm -lpthread >>"${WORK}/cc.log" 2>&1; then
    fail "driver does not build against ${S}"
    sed 's/^/      /' "${WORK}/cc.log" | head -20
    echo ""; echo "C200: ${PASS} passed, ${FAIL} failed"; exit 1
fi
mkdir -p "${WORK}/out"
if ! "${WORK}/drv" "${WORK}/out" >"${WORK}/run.log" 2>&1; then
    fail "driver aborted (sanitizer or crash)"
    grep -a -E 'ERROR|runtime error|SUMMARY' "${WORK}/run.log" | head -5 | sed 's/^/      /'
fi

# Each check prints "OK <what>" or "BAD <what>".
python3 - "${WORK}/out" > "${WORK}/py.log" 2>&1 <<'PY'
import json, os, sys, xml.etree.ElementTree as ET
d = sys.argv[1]
def rd(n):
    p = os.path.join(d, n)
    return open(p, encoding="utf-8").read() if os.path.exists(p) else ""
def ok(c, what): print(("OK " if c else "BAD ") + what)
def xml_ok(txt):
    try: return ET.fromstring(txt)
    except Exception: return None

s = rd("svg_hostile.svg")
t = xml_ok(s)
ok(t is not None, "svg: hostile document is well-formed XML")
ok("<script" not in s, "svg: no <script> element injected (text, path, ellipse, stop, gradient id)")
ok(t is not None and not any(k in ("onload", "onclick") for e in t.iter() for k in e.attrib),
   "svg: no attribute injected through fill/transform")
if t is not None:
    ns = "{http://www.w3.org/2000/svg}"
    txt = t.find(ns + "text")
    ok(txt is not None and txt.text == "T</text><script>alert(2)</script>&amp;",
       "svg: text content round-trips exactly through the parser")
    ok(txt is not None and 'fill:red" onload="alert(1)' in (txt.get("style") or ""),
       "svg: hostile fill round-trips as the style value")
else:
    ok(False, "svg: text content round-trips exactly through the parser")
    ok(False, "svg: hostile fill round-trips as the style value")
ok(rd("svg_plain.svg") ==
   '<svg xmlns="http://www.w3.org/2000/svg" width="10.00" height="10.00" viewBox="0 0 10.00 10.00">'
   '<text x="1.00" y="2.00" style="fill:#333;stroke:none;stroke-width:0.50">Hello world</text>'
   '<path d="M0 0 L10 10" style="fill:none;stroke:black;stroke-width:1.00"/></svg>',
   "svg: ordinary output byte-identical to the pre-escaping markup")

h = rd("canvas_hostile.html")
body = h.split("<script>\n", 1)[1] if "<script>\n" in h else ""
ok(body.count("</script") == 1 and body.rstrip().endswith("</script>"),
   "canvas: exactly one </script>, the wrapper's own")
from html.parser import HTMLParser
class P(HTMLParser):
    def __init__(s): super().__init__(); s.tags = []
    def handle_starttag(s, tag, attrs): s.tags.append((tag, dict(attrs)))
pp = P(); pp.feed(h.split("<script>", 1)[0])
ok(pp.tags == [("canvas", {"id": "cv\"><script>alert(1)</script><x id='", "width": "10", "height": "10"})],
   "canvas: id cannot leave its HTML attribute (one <canvas>, id round-trips)")
plain = rd("canvas_plain.js")
ok(plain == "const _c = document.getElementById('chart1').getContext('2d');\n"
   "_c.font='12px sans-serif'; _c.fillStyle='#333'; _c.fillText('Total 42',1,2);\n"
   "_c.fillStyle='rgba(0,0,0,0.5)'; _c.fillRect(0,0,5,5);\n"
   "_c.drawImage(new Image()/* src=logo.png */,0,0,4,4);\n",
   "canvas: ordinary output byte-identical to the pre-escaping JavaScript")

for n in ("chart.json", "chart_vega.json", "chart_theme.json"):
    try: json.loads(rd(n)); good = True
    except Exception: good = False
    ok(good, "chart: %s is valid JSON (control bytes, NaN, Inf)" % n)
try:
    cj = json.loads(rd("chart.json"))
    ok(cj["data"]["labels"] == ["a\x01b", "q\"x", "nl\nx"] and
       cj["options"]["plugins"]["title"]["text"] == "T\x02\"</script>",
       "chart: labels and title round-trip exactly")
    ok(cj["data"]["datasets"][0]["data"] == [1.5, None, None],
       "chart: NaN and Inf are written as null")
except Exception:
    ok(False, "chart: labels and title round-trip exactly")
    ok(False, "chart: NaN and Inf are written as null")
try:
    th = json.loads(rd("chart_theme.json"))
    ok("injected" not in th and th["theme_meta"]["theme"] == 'dark","injected":"1',
       "chart: set_theme cannot add keys; the theme name round-trips")
except Exception:
    ok(False, "chart: set_theme cannot add keys; the theme name round-trips")
hm = rd("heatmap.svg")
ok(xml_ok(hm) is not None and "<script" not in hm,
   "chart: heatmap title and labels are XML-escaped (well-formed, no <script>)")
PY
while IFS= read -r line; do
    case "${line}" in
        OK\ *)  pass "${line#OK }" ;;
        BAD\ *) fail "${line#BAD }" ;;
        *)      fail "python: ${line}" ;;
    esac
done < "${WORK}/py.log"

# The canvas JavaScript must parse, and running it against a stub canvas
# must never call alert().  node is optional on CI images.
if command -v node >/dev/null 2>&1; then
    cat > "${WORK}/run.js" <<'JS'
const fs = require('fs');
const src = fs.readFileSync(process.argv[2], 'utf8');
let alerted = 0, texts = [];
const ctx = new Proxy({}, { get: (t, k) => k === 'fillText' ? (s) => texts.push(s) : (t[k] || (() => {})),
                            set: (t, k, v) => { t[k] = v; return true; } });
const document = { getElementById: (id) => ({ getContext: () => ctx, _id: id }) };
function Image() {}
function alert() { alerted++; }
try { new Function('document', 'Image', 'alert', src)(document, Image, alert); }
catch (e) { console.log('THROW ' + e.message); process.exit(0); }
console.log('ALERTS ' + alerted);
console.log('TEXT ' + JSON.stringify(texts[0]));
JS
    out="$(node "${WORK}/run.js" "${WORK}/out/canvas_hostile.js" 2>&1)"
    if printf '%s\n' "${out}" | grep -q '^ALERTS 0$'; then
        pass "canvas: the hostile JavaScript parses and runs without calling alert()"
    else
        fail "canvas: hostile JavaScript: $(printf '%s' "${out}" | head -2 | tr '\n' ' ')"
    fi
    if printf '%s\n' "${out}" | grep -qF 'TEXT "'"');alert(2);//</script><script>alert(3)</script>"'"'; then
        pass "canvas: fillText receives the planted text exactly"
    else
        fail "canvas: fillText text not preserved: $(printf '%s' "${out}" | grep TEXT)"
    fi
else
    echo "  SKIP: node not installed -- canvas JavaScript not executed (the static checks above still ran)"
fi

echo ""
echo "C200: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
