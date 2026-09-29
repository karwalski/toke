#!/usr/bin/env bash
# C383_html_svg_record_fields.sh — std.html's $htmldoc/$htmlnode and
# std.svg's $svgdoc/$svgstyle read what their names say (story 135.30).
#
# THE DEFECT.  tkc reads `x.f` as 8-byte slot <index of f in the .tki>.
#   - $htmlnode is {tag; attrs:[[str]]; content}; html.h1/html.table
#     returned the C TkHtmlNode, whose slot 1 is a linked attribute list:
#     `n.attrs.len` crashed (SIGSEGV).  $htmldoc.id read the title pointer.
#   - $svgdoc is {width; height; viewbox; elements}; the C TkSvgDoc has the
#     element array in slot 2 and its count in slot 3: `.viewbox` printed
#     the array and `.elements.len` crashed.
#
# THE FIX (html.h/.c, svg.h/.c, svg_glue.c).  TkHtmlNode starts with {tag;
# attrs (toke [[str]] kept beside the C list); content (never NULL)};
# TkHtmlDoc and TkSvgDoc start with a head struct holding their record
# (svg_append keeps .elements current, .viewbox is what svg.render
# writes).  $svgstyle is TkSvgStyleRec for svg.style and literals alike.
# All marked; make check-tki-layout checks them (C389).  That .attrs holds
# @(name; value) pairs is asserted in C (make test-stdlib-html, case 40):
# no html constructor with glue sets an attribute.
#
# WHAT IS ASSERTED:
#   1. html.doc: two docs read ids 1 and 2
#   2. html.h1: .tag, .content, .attrs (empty); html.table: content "" not
#      NULL, attrs empty
#   3. svg.style: .fill, .stroke, .strokewidth
#   4. svg.doc + two svg.append: .width, .height, .viewbox, .elements.len
#   5. svg.render still writes both elements, with the literal $svgstyle
#   6. NEGATIVE CONTROL: another svg.doc reads its own size and no elements
#
# C383_STDLIB=<dir> builds against another stdlib tree.  Against main
# 9f901bf: 0/9 -- the program dies with SIGSEGV.
#
# Story: 135.30
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C383_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c383_XXXXXX")"
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
echo "C383: std.html and std.svg record fields (135.30)"
echo "------------------------------------------------------------"
cat > p.tk <<'TK'
m=p;
i=io:std.io;
i=str:std.str;
i=html:std.html;
i=svg:std.svg;
f=main():i64{
  let d=html.doc();
  let d2=html.doc();
  io.println(str.concat(str.fromint(d.id as i64); str.concat(" "; str.fromint(d2.id as i64))));
  let h=html.h1("Title");
  io.println(str.concat(h.tag; str.concat(" "; str.concat(h.content; str.concat(" "; str.fromint(h.attrs.len as i64))))));
  let t=html.table(@("a";"b");@(@("1";"2")));
  io.println(str.concat(t.tag; str.concat(" ["; str.concat(t.content; str.concat("] "; str.fromint(t.attrs.len as i64))))));
  let st=svg.style("red";"black";2.0);
  io.println(str.concat(st.fill; str.concat(" "; str.concat(st.stroke; str.concat(" "; str.fromf64(st.strokewidth))))));
  let s=svg.doc(100.0;50.0);
  let s2=svg.append(s;svg.rect(1.0;2.0;3.0;4.0;st));
  let s3=svg.append(s2;svg.circle(5.0;5.0;2.0;$svgstyle{fill:"blue";stroke:"";strokewidth:1.0;opacity:0.5;fontsize:0.0;fontfamily:""}));
  io.println(str.concat(str.fromf64(s3.width); str.concat(" "; str.concat(str.fromf64(s3.height); str.concat(" "; s3.viewbox)))));
  io.println(str.fromint(s3.elements.len as i64));
  io.println(svg.render(s3));
  let o=svg.doc(7.0;8.0);
  io.println(str.concat(o.viewbox; str.concat(" "; str.fromint(o.elements.len as i64))));
  <0
};
TK
got="$(run p)"
f() { printf '%s' "${got% rc=*}" | cut -d'|' -f"$1"; }
expect "1  html.doc: ids 1 and 2" "$(f 1)" "1 2"
expect "2a html.h1: tag, content, attrs" "$(f 2)" "h1 Title 0"
expect "2b html.table: content \"\", attrs empty" "$(f 3)" "table [] 0"
expect "3  svg.style: fill, stroke, strokewidth" "$(f 4)" "red black 2"
expect "4a svg.doc: width, height, viewbox" "$(f 5)" "100 50 0 0 100.00 50.00"
expect "4b .elements after two appends" "$(f 6)" "2"
expect "5  svg.render: both elements, the literal style applied" \
    "$(f 7 | grep -c 'viewBox="0 0 100.00 50.00"><rect x="1.00".*fill:red;stroke:black.*<circle.*fill:blue.*opacity:0.50')" "1"
expect "6  NEGATIVE CONTROL: another svg.doc reads its own fields" "$(f 8)" "0 0 7.00 8.00 0"
expect "   exit status" "${got##* }" "rc=0"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
