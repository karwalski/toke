#!/usr/bin/env bash
# C1061_vecstore_template_router_sealed.sh -- std.vecstore's search results
# are sealed; std.vecstore, std.template, std.router and the std.http web
# glue read lengths with tk_strlen (story 127.295, S1 step 5 tranche 5;
# runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen):
#        - vecstore: a collection named by a sealed string, upserts with
#          sealed ids/payloads, search through the toke glue
#          (tk_vecstore_search_w): every result .id and .payload is sealed
#          with header == strlen and the expected text, the top hit first;
#          a flush writes the collection file;
#        - template: tmpl_compile / tmpl_renderhtml / tmpl_html / tmpl_escape
#          over sealed sources, keys, values, tags and children give the
#          exact expected bytes;
#        - router: router_query_get with a sealed query and key.
#   2. toke, under TK_STR_VERIFY=1: vecstore.search results' .id / .payload
#      have the expected text and `.len`, and the program exits 0.
#
# Negative control: C1061_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   main 034332b8's src/stdlib + stdlib: 14 passed, 4 failed -- the four
#   search-result strings carry no header ("not sealed"); everything else
#   passes there, because an unsealed string read with strlen was always
#   correct (the reader conversion is a speed change, gated by the ratchet).
# Measured: this branch 18 passed (14 C + 4 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1061_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1061_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1061: vecstore results sealed; vecstore/template/router read with tk_strlen (127.295)"
echo "----------------------------------------------------------------------"

# ── 1. C ─────────────────────────────────────────────────────────────────────
mkdir -p vsdata
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include "tk_strhdr.h"
#include "tk_array.h"
#include "vecstore.h"
#include "template.h"
#include "router.h"
int64_t tk_vecstore_search_w(int64_t coll, int64_t query_vec, int64_t k, int64_t min_score_bits);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int sealed_eq(const char *s, const char *want)
{
    if (!s) return 0;
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%s')\n", s); return 0; }
    return (size_t)h == strlen(s) && strcmp(s, want) == 0;
}
static const char *S(const char *s) { return tks_strdup(s); }
int main(int argc, char **argv)
{
    (void)argc;
    /* vecstore */
    TkVecStore *vs = vecstore_open(S(argv[1]));
    CHECK(vs != NULL, "vecstore_open on a sealed path");
    TkVecCollection *c = vecstore_collection(vs, S("docs"));
    CHECK(c != NULL, "vecstore_collection on a sealed name");
    float a[3] = { 1, 0, 0 }, b[3] = { 0, 1, 0 };
    CHECK(vecstore_upsert(c, S("doc-1"), a, 3, S("payload-one")) &&
          vecstore_upsert(c, S("doc-22"), b, 3, S("payload two!")),
          "vecstore_upsert with sealed ids and payloads");
    int64_t q = tk_arr_alloc(3, 3);
    double qd[3] = { 1.0, 0.25, 0.0 };
    for (int i = 0; i < 3; i++) memcpy(&((int64_t *)(intptr_t)q)[i], &qd[i], 8);
    double ms = 0.0; int64_t msb; memcpy(&msb, &ms, 8);
    int64_t r = tk_vecstore_search_w((int64_t)(intptr_t)c, q, 5, msb);
    CHECK(tk_arr_len(r) == 2, "search returns both entries");
    if (tk_arr_len(r) == 2) {
        int64_t *r0 = (int64_t *)(intptr_t)((int64_t *)(intptr_t)r)[0];
        int64_t *r1 = (int64_t *)(intptr_t)((int64_t *)(intptr_t)r)[1];
        CHECK(sealed_eq((const char *)(intptr_t)r0[0], "doc-1"), "result[0].id sealed, header == strlen");
        CHECK(sealed_eq((const char *)(intptr_t)r0[2], "payload-one"), "result[0].payload sealed, header == strlen");
        CHECK(sealed_eq((const char *)(intptr_t)r1[0], "doc-22"), "result[1].id sealed, header == strlen");
        CHECK(sealed_eq((const char *)(intptr_t)r1[2], "payload two!"), "result[1].payload sealed, header == strlen");
    }
    CHECK(vecstore_flush(vs) == 1, "vecstore_flush (collection_save reads sealed ids/payloads)");
    struct stat st; char path[512];
    snprintf(path, sizeof path, "%s/docs.vecs", argv[1]);
    CHECK(stat(path, &st) == 0 && st.st_size > 0, "the collection file was written");

    /* template */
    TkTmpl *t = tmpl_compile(S("<p title=\"{{t}}\">{{b}}</p>{{r|raw}}"));
    TkTmplVar v[3] = { { S("t"), S("a\"b") }, { S("b"), S("<x>&") }, { S("r"), S("<i>") } };
    const char *out = t ? tmpl_renderhtml(t, v, 3) : NULL;
    CHECK(out && strcmp(out, "<p title=\"a&quot;b\">&lt;x&gt;&amp;</p><i>") == 0,
          "tmpl_renderhtml over sealed source, keys and values");
    const char *ak[1] = { S("class") }, *av[1] = { S("c1") }, *ch[2] = { S("one"), S("two") };
    const char *h = tmpl_html(S("div"), ak, av, 1, ch, 2);
    CHECK(h && strcmp(h, "<div class=\"c1\">onetwo</div>") == 0, "tmpl_html over sealed tag, attrs, children");
    const char *e = tmpl_escape(S("<a>"));
    CHECK(e && strcmp(e, "&lt;a&gt;") == 0, "tmpl_escape over a sealed string");

    /* router */
    const char *qv = router_query_get(S("x=1&name=al%20ice&y=2"), S("name"));
    CHECK(qv && strcmp(qv, "al ice") == 0, "router_query_get with a sealed query and key");
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects vecstore template router -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${WORK}/vsdata" > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
mkdir -p tkdata
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=vs:std.vecstore;
i=env:std.env;
f=main():i64{
  let s=vs.open(env.getor("VSDIR";"."));
  let c=vs.collection(s;"docs");
  let ok1=vs.upsert(c;"doc-1";@(1.0;0.0;0.0);"payload-one");
  let ok2=vs.upsert(c;"doc-22";@(0.0;1.0;0.0);"payload two!");
  let r=vs.search(c;@(1.0;0.25;0.0);5;0.0);
  let a=r.get(0);
  let b=r.get(1);
  io.println("id0=[\(a.id)] \(a.id.len)");
  io.println("pay0=[\(a.payload)] \(a.payload.len)");
  io.println("id1=[\(b.id)] \(b.id.len)");
  io.println("pay1=[\(b.payload)] \(b.payload.len)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    VSDIR="${WORK}/tkdata" TK_STR_VERIFY=1 ./p --allow-read --allow-write > p.out 2>&1; rc=$?
    [ "${rc}" -eq 0 ] || fail "toke program exited ${rc}: $(head -c 300 p.out)"
    for want in "id0=[doc-1] 5" "pay0=[payload-one] 11" "id1=[doc-22] 6" "pay1=[payload two!] 12"; do
        if grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}', got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
fi

echo "----------------------------------------------------------------------"
echo "C1061: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
