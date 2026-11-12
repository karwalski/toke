#!/usr/bin/env bash
# C1230_template_sealed.sh -- std.template's producers hand out sealed
# strings, and their C callers no longer free them (story 127.295, S1 step 5
# tranche 8; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen):
#        - tmpl_render, tmpl_renderhtml, tmpl_escape, tmpl_html and
#          tmpl_renderfile return sealed strings, header == strlen, with the
#          exact expected bytes (escaping unchanged); an empty render is a
#          sealed "";
#        - the toke glue: tpl.compile's $tmpl.src (tk_template_compile_w),
#          tpl.render (tk_template_render_w), tpl.renderfile
#          (tk_template_renderfile_w) and tpl.escape (tk_template_escape_w)
#          return sealed strings;
#        - tmpl_renderpage is NOT sealed: it stays malloc'd because
#          tk_page_dispatch renders one per request and frees it (a sealed
#          string is never freed); the harness frees it under ASan, with and
#          without a layout.
#   2. toke, under TK_STR_VERIFY=1: tpl.compile/.src, tpl.render,
#      tpl.renderfile and tpl.escape give the expected text and `.len`, and
#      the program exits 0.
#
# Negative control: C1230_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   main 031d72fd's src/stdlib + stdlib: 6 passed, 10 failed -- the ten
#   producer/glue strings carry no header ("not sealed"); the renderpage and
#   five toke cases pass there, because an unsealed string read with strlen
#   was always correct.
# Measured: this branch 16 passed (11 C + 5 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1230_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1230_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1230: std.template results sealed; renderpage stays malloc'd (127.295)"
echo "----------------------------------------------------------------------"

mkdir -p tpl
printf '<p>{{v}}</p>' > tpl/file.tkt
printf '<html>{! yield("b") !}</html>' > tpl/base.tkt
printf '{! layout("base") !}{! block("b") !}<i>{{v}}</i>{! end !}' > tpl/page.tkt
printf 'plain {{v}}.' > tpl/nolayout.tkt

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "template.h"
int64_t tk_template_compile_w(int64_t src);
int64_t tk_template_render_w(int64_t tpl, int64_t data);
int64_t tk_template_renderfile_w(int64_t path, int64_t data);
int64_t tk_template_escape_w(int64_t s);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || strcmp(s, want) != 0)
        printf("        (got '%s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && strcmp(s, want) == 0;
}
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char path[1024];
    TkTmplVar v[2] = { { "v", "<a&b>" }, { "w", "x\"y" } };

    TkTmpl *t = tmpl_compile("<p title=\"{{w}}\">{{v}}</p>");
    CHECK(sealed_eq(tmpl_render(t, v, 2), "<p title=\"x\"y\"><a&b></p>"),
          "tmpl_render: sealed, raw values");
    CHECK(sealed_eq(tmpl_renderhtml(t, v, 2), "<p title=\"x&quot;y\">&lt;a&amp;b&gt;</p>"),
          "tmpl_renderhtml: sealed, escaped for context");
    tmpl_free(t);
    TkTmpl *e = tmpl_compile("");
    CHECK(sealed_eq(tmpl_renderhtml(e, NULL, 0), ""), "tmpl_renderhtml of an empty template: sealed \"\"");
    tmpl_free(e);
    CHECK(sealed_eq(tmpl_escape("<'&\">"), "&lt;&#39;&amp;&quot;&gt;"), "tmpl_escape: sealed");
    const char *ak[1] = { "id" }, *av[1] = { "a<b" }, *ch[2] = { "x", "<y/>" };
    CHECK(sealed_eq(tmpl_html("div", ak, av, 1, ch, 2), "<div id=\"a&lt;b\">x<y/></div>"),
          "tmpl_html: sealed");
    snprintf(path, sizeof path, "%s/file.tkt", dir);
    CHECK(sealed_eq(tmpl_renderfile(path, v, 1), "<p>&lt;a&amp;b&gt;</p>"), "tmpl_renderfile: sealed");

    /* the toke glue */
    int64_t rec = tk_template_compile_w(I("Hi {{v}}!"));
    CHECK(rec && sealed_eq(((const TkTmplRec *)(intptr_t)rec)->src, "Hi {{v}}!"),
          "tpl.compile: $tmpl.src sealed");
    CHECK(rec && sealed_eq(P(tk_template_render_w(rec, 0)), "Hi !"), "tpl.render (glue): sealed");
    CHECK(sealed_eq(P(tk_template_renderfile_w(I(path), 0)), "<p></p>"), "tpl.renderfile (glue): sealed");
    CHECK(sealed_eq(P(tk_template_escape_w(I("a<b"))), "a&lt;b"), "tpl.escape (glue): sealed");

    /* tmpl_renderpage: malloc'd, the C caller frees it (tk_page_dispatch) */
    snprintf(path, sizeof path, "%s/page.tkt", dir);
    char *pg = (char *)tmpl_renderpage(path, dir, v, 1);
    snprintf(path, sizeof path, "%s/nolayout.tkt", dir);
    char *pn = (char *)tmpl_renderpage(path, dir, v, 1);
    CHECK(pg && strcmp(pg, "<html><i>&lt;a&amp;b&gt;</i></html>") == 0 && tks_hdrlen(pg) < 0 &&
          pn && strcmp(pn, "plain &lt;a&amp;b&gt;.") == 0 && tks_hdrlen(pn) < 0,
          "tmpl_renderpage (layout and none): malloc'd, not sealed, and free()d");
    free(pg); free(pn);
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects template -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${WORK}/tpl" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=tpl:std.template;
f=main():i64{
  let t=mt tpl.compile("Hi {{name}}!") {$ok:v v;$err:e $tmpl{id:0;src:""}};
  io.println("src=[\(t.src)] \(t.src.len)");
  let v=tpl.vars(@("name":"<Eve>";"v":"a&b"));
  let r=mt tpl.render(t;v) {$ok:s s;$err:e "err"};
  io.println("render=[\(r)] \(r.len)");
  let f=mt tpl.renderfile("tpl/file.tkt";v) {$ok:s s;$err:e "err"};
  io.println("file=[\(f)] \(f.len)");
  let e=tpl.escape("<'>");
  io.println("escape=[\(e)] \(e.len)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p --allow-read > p.out 2>&1; rc=$?
    if [ "${rc}" -eq 0 ]; then pass "toke program exits 0 under TK_STR_VERIFY=1"
    else fail "toke program exited ${rc}: $(head -c 300 p.out)"; fi
    for want in "src=[Hi {{name}}!] 12" "render=[Hi &lt;Eve&gt;!] 15" \
                "file=[<p>a&amp;b</p>] 14" "escape=[&lt;&#39;&gt;] 13"; do
        if grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}', got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
fi

echo "----------------------------------------------------------------------"
echo "C1230: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
