#!/usr/bin/env bash
# C910_render_result_survives_rerender.sh -- a string returned by html.render,
# html.docr or svg.render stays valid after the document is rendered again
# (story 127.316, found while converting std.html/std.svg for 127.295).
#
# THE DEFECT.  html_render() and svg_render() cache their output in the
# document (doc->rendered) and free the previous buffer at the start of the
# next render.  The glue returned that cached pointer as the toke string, and
# toke strings are never freed or tracked (runtime-abi §3) -- so after
#     let a=html.render(d); html.title(d;"..."); let b=html.render(d);
# `a` pointed into freed memory: on main it printed as "" with a.len=0 (the
# allocator had reused the block), or as the new document's bytes.
#
# THE FIX.  tk_html_render_w, tk_html_docr_w and tk_svg_render_w return their
# own copy of the rendered text, a sealed string (127.295, runtime-abi §3.1).
#
# WHAT IS ASSERTED, from a compiled program run under TK_STR_VERIFY=1 (every
# trusted length read checks header == strlen and aborts otherwise):
#   1. html: after a second render, the first result still has its own
#      length and contents (the old title, not the new one);
#   2. svg: the same, a one-rect render kept across a two-rect render;
#   3. html.docr (an i64 ABI helper in html.tki) still returns the page with
#      the paragraph it was given;
#   4. the program exits 0 (no verify abort, no free() abort).
#
# Negative control: C910_STDLIB=<dir> compiles against another tree's
# src/stdlib.  Against a git archive of main's src/stdlib (7758dcf3) the
# html and svg cases print a.len=0 / x.len=0: 2 passed, 2 failed.
#
# Story: 127.316 (127.295)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C910_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c910_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C910: html.render / svg.render results survive a re-render (127.316)"
echo "----------------------------------------------------------------------"

cat > r.tk <<'TOKE'
m=main;
i=io:std.io;
i=str:std.str;
i=html:std.html;
i=svg:std.svg;
f=main():i64{
  let d=html.doc();
  html.title(d;"first");
  let a=html.render(d);
  let alen=a.len;
  html.title(d;"second-title-that-is-much-longer-than-the-first-one-so-the-buffer-moves");
  let b=html.render(d);
  io.println("html a.len=\(a.len) same=\(a.len==alen) old=\(str.contains(a;"<title>first</title>")) new=\(str.contains(b;"second-title"))");
  let st=svg.style("red";"blue";1.0);
  let s=svg.doc(10.0;10.0);
  svg.append(s;svg.rect(0.0;0.0;1.0;1.0;st));
  let x=svg.render(s);
  let xlen=x.len;
  svg.append(s;svg.rect(2.0;2.0;3.0;3.0;st));
  let y=svg.render(s);
  io.println("svg x.len=\(x.len) same=\(x.len==xlen) grew=\(y.len>x.len)");
  let p=html.docr("hello-docr");
  io.println("docr=\(str.contains(p;"hello-docr"))");
  <0
};
TOKE

if ! "${TKC}" --allow-all --out r r.tk > r.log 2>&1; then
    fail "r.tk did not compile"
    sed 's/^/        /' r.log | head -6
else
    TK_STR_VERIFY=1 ./r > r.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] && pass "program ran to exit 0 under TK_STR_VERIFY=1" \
                      || fail "program exited ${rc}: $(head -c 300 r.out)"
    if grep -a -q -E '^html a\.len=[1-9][0-9]* same=true old=true new=true$' r.out; then
        pass "html: first render kept its length and text after a re-render"
    else
        fail "html: first render not kept: '$(grep -a '^html' r.out)'"
    fi
    if grep -a -q -E '^svg x\.len=[1-9][0-9]* same=true grew=true$' r.out; then
        pass "svg: first render kept its length after a re-render"
    else
        fail "svg: first render not kept: '$(grep -a '^svg' r.out)'"
    fi
    if grep -a -q -x -F 'docr=true' r.out; then
        pass "html.docr returns the rendered paragraph"
    else
        fail "html.docr: '$(grep -a '^docr' r.out)'"
    fi
fi

echo "----------------------------------------------------------------------"
echo "C910: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
