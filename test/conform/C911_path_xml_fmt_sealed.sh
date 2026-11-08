#!/usr/bin/env bash
# C911_path_xml_fmt_sealed.sh -- std.path, std.xml and std.fmt return sealed
# strings (story 127.295, S1 step 5; docs/runtime-abi.md §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen.  This tranche converted every
# string producer of path.c, xml.c's public API and fmt.c.
#
# WHAT IS ASSERTED:
#   1. C: every converted producer, called on ordinary and edge inputs (empty,
#      "/", dotfiles, a missing attribute's NULL aside, entity unescape,
#      zero-element joins, no-op pads), returns a string whose header is
#      present (tks_hdrlen >= 0) and equals strlen -- the trusted length is
#      the C-string length.  One line per producer call.
#   2. toke, under TK_STR_VERIFY=1 (every trusted length read asserts
#      header == strlen and aborts otherwise): `.len` and the text of each
#      producer's result, computed in the program, match the expected values,
#      and the program exits 0.
#
# Negative control: C911_STDLIB=<dir> uses another tree's src/stdlib.  Against
# a git archive of main's src/stdlib (7758dcf3) every C case reports "no
# header" (none of these producers sealed); the toke case still passes there,
# since an unsealed string is read with strlen -- correctness never depended
# on the header, which is the point of S1.
# Measured: this branch 47 passed (32 C + 15 toke); main 15 passed, 32
# failed (every C case "no header").
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C911_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c911_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C911: std.path / std.xml / std.fmt results are sealed strings (127.295)"
echo "----------------------------------------------------------------------"

# ── 1. C: the header is present and equals strlen ───────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include "path.h"
#include "xml.h"
#include "fmt.h"
#include "tk_strhdr.h"

static void chk(const char *what, const char *r)
{
    if (!r) { printf("FAIL %s: NULL\n", what); return; }
    int64_t h = tks_hdrlen(r);
    if (h < 0)                         printf("FAIL %s: no header\n", what);
    else if ((size_t)h != strlen(r))   printf("FAIL %s: header %lld != strlen %zu\n",
                                              what, (long long)h, strlen(r));
    else                               printf("ok %s\n", what);
}

int main(void)
{
    chk("path_join",         path_join("a/", "/b"));
    chk("path_join empty",   path_join("", ""));
    chk("path_normalize",    path_normalize("/a/./b/../c//d"));
    chk("path_normalize .",  path_normalize("x/.."));
    chk("path_ext",          path_ext("x/foo.tar.gz"));
    chk("path_ext none",     path_ext("foo"));
    chk("path_ext dotfile",  path_ext(".hidden"));
    chk("path_stem",         path_stem("x/foo.tar.gz"));
    chk("path_stem dotfile", path_stem(".hidden"));
    chk("path_stem plain",   path_stem("foo"));
    chk("path_dir",          path_dir("/usr/lib/x"));
    chk("path_dir none",     path_dir("foo"));
    chk("path_dir root",     path_dir("/foo"));
    chk("path_base",         path_base("/usr/lib/"));
    chk("path_base root",    path_base("/"));
    chk("xml_element",       xml_element("a", "t", "k=\"v\""));
    chk("xml_element bare",  xml_element("a", "", ""));
    chk("xml_element_self",  xml_element_self_closing("br", "x=\"1\""));
    chk("xml_element_ns",    xml_element_ns("s", "a", "t", ""));
    chk("xml_escape",        xml_escape("<a&'\">"));
    chk("xml_escape NULL",   xml_escape(NULL));
    chk("xml_cdata",         xml_cdata("x<y"));
    chk("xml_declaration",   xml_declaration("1.0", "UTF-8"));
    chk("xml_comment",       xml_comment("n"));
    chk("xml_get",           xml_get("<r><n>J</n></r>", "r.n"));
    chk("xml_attr",          xml_attr("href=\"a&amp;b\" id='7'", "href"));
    int64_t xs[3] = { -1, 0, 9223372036854775807LL };
    chk("fmt_join_i64",      fmt_join_i64(xs, 3, ", "));
    chk("fmt_join_i64 none", fmt_join_i64(xs, 0, ", "));
    const char *ss[3] = { "a", NULL, "ccc" };
    chk("fmt_join_str",      fmt_join_str(ss, 3, "-"));
    chk("fmt_pad left",      fmt_pad("h\xc3\xa9", 5, 1));
    chk("fmt_pad right",     fmt_pad("abc", 5, 0));
    chk("fmt_pad no-op",     fmt_pad("abcdef", 3, 0));
    return 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
CF=(-std=c99 -D_GNU_SOURCE -Wall -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" --objects path xml fmt -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lpthread > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] || fail "C harness exited ${rc}"
    while IFS= read -r line; do
        case "${line}" in
            ok\ *)   pass "C: ${line#ok } sealed, header == strlen" ;;
            FAIL\ *) fail "C: ${line#FAIL }" ;;
            *)       fail "C: unexpected output: ${line}" ;;
        esac
    done < seal.out
fi

# ── 2. toke: .len and text under TK_STR_VERIFY=1 ────────────────────────────
cat > p.tk <<'TOKE'
m=main;
i=io:std.io;
i=path:std.path;
i=xml:std.xml;
i=fmt:std.fmt;
f=main():i64{
  let j=path.join("a/";"/b");
  io.println("join=[\(j)] \(j.len)");
  let n=path.normalize("/a/./b/../c//d");
  io.println("normalize=[\(n)] \(n.len)");
  let e=path.ext("x/foo.tar.gz");
  io.println("ext=[\(e)] \(e.len)");
  let s=path.stem("x/foo.tar.gz");
  io.println("stem=[\(s)] \(s.len)");
  let d=path.dir("/usr/lib/x");
  io.println("dir=[\(d)] \(d.len)");
  let b=path.base("/usr/lib/");
  io.println("base=[\(b)] \(b.len)");
  let x=xml.element("a";"t";"k=\"v\"");
  io.println("element=[\(x)] \(x.len)");
  let es=xml.escape("<&>");
  io.println("escape=[\(es)] \(es.len)");
  let g=xml.get("<r><n>John</n></r>";"r.n");
  io.println("get=[\(g)] \(g.len)");
  let at=xml.attr("href=\"a&amp;b\"";"href");
  io.println("attr=[\(at)] \(at.len)");
  let fa=fmt.arr(@(1;-2;3);",");
  io.println("arr=[\(fa)] \(fa.len)");
  let fs=fmt.strs(@("x";"yy");"+");
  io.println("strs=[\(fs)] \(fs.len)");
  let fp=fmt.pad("ab";5;true);
  io.println("pad=[\(fp)] \(fp.len)");
  let ff=fmt.f64(3.14159;2);
  io.println("f64=[\(ff)] \(ff.len)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] && pass "toke program ran to exit 0 under TK_STR_VERIFY=1" \
                      || fail "toke program exited ${rc}: $(head -c 300 p.out)"
    for want in "join=[a/b] 3" \
                "normalize=[/a/c/d] 6" \
                "ext=[.gz] 3" \
                "stem=[foo.tar] 7" \
                "dir=[/usr/lib] 8" \
                "base=[lib] 3" \
                "element=[<a k=\"v\">t</a>] 14" \
                "escape=[&lt;&amp;&gt;] 13" \
                "get=[John] 4" \
                "attr=[a&b] 3" \
                "arr=[1,-2,3] 6" \
                "strs=[x+yy] 4" \
                "pad=[   ab] 5" \
                "f64=[3.14] 4"; do
        if grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}', got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
fi

echo "----------------------------------------------------------------------"
echo "C911: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
