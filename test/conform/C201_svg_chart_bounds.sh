#!/usr/bin/env bash
# C201_svg_chart_bounds.sh — svg_elem_set_style never frees caller-owned
# strings; chart_histogram and chart_heatmap stay inside their input and
# inside defined behaviour (stories 122.13 (2)(3), 135.3j (8)).
#
# THE DEFECTS (measured on main before 122.13).
#   svg_elem_set_style  free()d elem->style.fill/stroke/font_family before
#                       replacing them -- but svg_style() stores the CALLER's
#                       pointers, so the first set_style after
#                       svg_style("red", ...) freed a string literal (ASan:
#                       bad-free).  Its own strdup'd copies were never freed
#                       by svg_elem_free either (a leak).
#   chart_histogram     read values[0] when n == 0 (past the input: ASan
#                       heap-buffer-overflow on a zero-length array); a NaN
#                       reached (uint64_t)((v - min) / width), undefined
#                       behaviour (UBSan float-cast-overflow).
#   chart_heatmap       nrows * ncols wrapped (the min/max scan then read far
#                       past the matrix), and (int)ncols * 60 overflowed int
#                       for ncols past ~35.8M; a NaN cell reached (int)NaN.
#
# WHAT THIS DRIVES (ASan+UBSan, -fno-sanitize-recover, leak detection on for
# the svg case): each scenario runs in its own process so one abort cannot
# hide the next, and each asserts a RESULT as well as a clean exit
# (set_style's values rendered; the histogram JSON valid with the right
# counts; the refused heatmaps NULL; the NaN heatmap well-formed XML).
#
# Negative control: C201_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's svg.c and chart.c fails all six scenarios.
#
# Story: 122.13, 135.3j

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C201_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c201_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"

echo "C201: svg set_style ownership; chart histogram/heatmap bounds"
echo "-------------------------------------------------------------"

if ! command -v python3 >/dev/null 2>&1; then
    echo "SKIP: python3 not found (needed to parse the chart output)"
    echo "CONFORM-SKIP: python3 not installed"
    exit 0
fi

cat > "${WORK}/drv.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "svg.h"
#include "chart.h"
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *w = argv[1];
    if (!strcmp(w, "svg_set_style")) {
        TkSvgDoc *d = svg_doc(10, 10);
        TkSvgElem *e = svg_rect(0, 0, 1, 1, svg_style("red", "blue", 1.0));
        svg_elem_set_style(e, "fill: green; stroke: black; font-family: serif");
        svg_elem_set_style(e, "fill: navy");            /* replaces its own copy */
        svg_append(d, e);
        puts(svg_render(d));
        svg_free(d);
        return 0;
    }
    if (!strcmp(w, "hist_empty")) {
        double *v = (double *)malloc(1);   /* a zero-length array: reading v[0] overflows */
        const char *j = chart_histogram(v, 0, 4, "empty");
        puts(j ? j : "NULL");
        free((void *)j); free(v);
        return 0;
    }
    if (!strcmp(w, "hist_nan")) {
        double v[5] = { 1.0, 0.0 / 0.0, 2.0, 1.0 / 0.0, 3.0 };
        const char *j = chart_histogram(v, 5, 2, "nan");
        puts(j ? j : "NULL");
        free((void *)j);
        return 0;
    }
    if (!strcmp(w, "heat_wrap")) {
        double m[4] = { 1, 2, 3, 4 };
        const char *r[1] = { "r" }, *c[1] = { "c" };
        /* (2^32+1) * (2^32-1) == 2^64-1: the product wraps */
        const char *s = chart_heatmap(r, 4294967295ULL, c, 4294967297ULL, m, "t");
        puts(s ? "NOT-NULL" : "NULL");
        return 0;
    }
    if (!strcmp(w, "heat_wide")) {
        double m[4] = { 1, 2, 3, 4 };
        const char *r[1] = { "r" }, *c[1] = { "c" };
        const char *s = chart_heatmap(r, 1, c, 40000000ULL, m, "t");   /* 40M * 60 px > INT_MAX */
        puts(s ? "NOT-NULL" : "NULL");
        return 0;
    }
    if (!strcmp(w, "heat_nan")) {
        double m[4] = { 1.0, 0.0 / 0.0, 3.0, 1.0 / 0.0 };
        const char *r[2] = { "a", "b" }, *c[2] = { "x", "y" };
        const char *s = chart_heatmap(r, 2, c, 2, m, "t");
        puts(s ? s : "NULL");
        free((void *)s);
        return 0;
    }
    return 2;
}
C

# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list.
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" --objects svg chart -- "${CF[@]}")" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o "${WORK}/drv" "${WORK}/drv.c" "${LINK[@]}" -lm -lz -lpthread \
        >"${WORK}/cc.log" 2>&1; then
    fail "driver does not build against ${S}"
    sed 's/^/      /' "${WORK}/cc.log" | head -20
    echo ""; echo "C201: ${PASS} passed, ${FAIL} failed"; exit 1
fi

# run SCENARIO ASAN_OPTIONS → output in ${WORK}/<scenario>.out; returns exit
run() {
    ASAN_OPTIONS="$2" "${WORK}/drv" "$1" >"${WORK}/$1.out" 2>"${WORK}/$1.err"
}
san_line() { grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error|ERROR: LeakSanitizer' "${WORK}/$1.err" | sed 's/^/      /'; }

# LeakSanitizer is not supported by Apple's clang runtime; use it where it is.
LEAK=0
if "${CC}" -fsanitize=address -x c - -o "${WORK}/lsan" >/dev/null 2>&1 <<<'int main(void){return 0;}' \
   && ASAN_OPTIONS=detect_leaks=1 sh -c '"$1" 2>/dev/null' _ "${WORK}/lsan" >/dev/null 2>&1; then LEAK=1; fi

if run svg_set_style "detect_leaks=${LEAK}:abort_on_error=0" \
   && grep -q 'style="fill:navy;stroke:black;stroke-width:1.00;font-family:serif"' "${WORK}/svg_set_style.out"; then
    pass "svg_elem_set_style on svg_style() strings: no bad free, values applied$([ ${LEAK} = 1 ] && echo ', no leak')"
else
    fail "svg_elem_set_style on caller-owned strings"; san_line svg_set_style
fi
[ "${LEAK}" = 1 ] || echo "  NOTE: LeakSanitizer unavailable here -- the set_style leak half is checked on Linux only"

if run hist_empty "detect_leaks=0:abort_on_error=0" && python3 - "${WORK}/hist_empty.out" <<'PY'
import json, sys
j = json.loads(open(sys.argv[1]).read())
d = j["data"]["datasets"][0]["data"]
sys.exit(0 if sum(d) == 0 and len(j["data"]["labels"]) == len(d) else 1)
PY
then pass "chart_histogram n == 0: stays inside the input, valid JSON, every bin 0"
else fail "chart_histogram n == 0"; san_line hist_empty; fi

if run hist_nan "detect_leaks=0:abort_on_error=0" && python3 - "${WORK}/hist_nan.out" <<'PY'
import json, sys
j = json.loads(open(sys.argv[1]).read())
sys.exit(0 if j["data"]["datasets"][0]["data"] == [1, 2] else 1)
PY
then pass "chart_histogram NaN/Inf: no UB, not binned; finite values counted [1, 2]"
else fail "chart_histogram NaN/Inf"; san_line hist_nan; fi

if run heat_wrap "detect_leaks=0:abort_on_error=0" && grep -qx NULL "${WORK}/heat_wrap.out"; then
    pass "chart_heatmap nrows*ncols that wraps: refused (NULL), matrix not read"
else fail "chart_heatmap wrapping size"; san_line heat_wrap; fi

if run heat_wide "detect_leaks=0:abort_on_error=0" && grep -qx NULL "${WORK}/heat_wide.out"; then
    pass "chart_heatmap 40M columns (pixel width > INT_MAX): refused (NULL)"
else fail "chart_heatmap int-overflowing width"; san_line heat_wide; fi

if run heat_nan "detect_leaks=0:abort_on_error=0" && python3 - "${WORK}/heat_nan.out" <<'PY'
import sys, xml.etree.ElementTree as ET
t = ET.fromstring(open(sys.argv[1]).read())
ns = "{http://www.w3.org/2000/svg}"
sys.exit(0 if [x.text for x in t.iter(ns + "text")].count("n/a") == 2 else 1)
PY
then pass "chart_heatmap NaN/Inf cells: no UB, drawn as n/a"
else fail "chart_heatmap NaN cells"; san_line heat_nan; fi

echo ""
echo "C201: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
