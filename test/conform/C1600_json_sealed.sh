#!/usr/bin/env bash
# C1600_json_sealed.sh -- std.json hands toke sealed strings, and json.c /
# json_glue.c read with tk_strlen (story 127.295, S1 step 5 tranche 12;
# runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
# json.c's own producers (json_enc, json_str, json_keys, json_at ...) stay
# malloc'd -- their C callers free them -- so the seal is made in the glue,
# at the boundary to toke.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), through the glue the compiler calls:
#        - json.enc / encstr / quote / stringify / encode: sealed, exact text;
#        - json.str and its aliases (getstr, getstring, getopt, trystr): a
#          decoded value sealed, "x\ny" kept whole;
#        - every element of json.keys, json.values, json.entries and
#          json.parsearray sealed;
#        - json.getval's raw text and json.strarr's array text sealed;
#        - the typed encoders encnum / encbool / encf64, encarr (and its
#          empty "[]") and encval (and its nil "null") sealed;
#        - json.serialize is NOT a copy: it is the document's own text
#          (Json.raw), by design -- the same pointer, unsealed;
#        - json.c's readers take sealed inputs: a sealed document text,
#          sealed keys and a sealed dotted path give the same answers as
#          unsealed ones (json_at, json_member, json_setmember, json_merge,
#          json_from_pairs, json_pretty).
#   2. toke, under TK_STR_VERIFY=1: json.dec -> str/keys/entries/getval,
#      json.enc of str/i64/bool/f64/array, json.strarr, json.serialize, and
#      `.len` of each.
#
# Negative control: C1600_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base 76574959's src/stdlib + stdlib: 4 passed, 8 failed -- dec, serialize,
#   the json.c readers and the toke case pass there (an unsealed string read
#   with strlen was always correct); every producer case reports "not sealed".
# Measured: this branch 12 passed (11 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1600_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1600_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1600: std.json results sealed; json.c / json_glue.c read with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
#include "tk_array.h"
#include "tk_strhdr.h"
int64_t tk_json_enc_w(int64_t v);
int64_t tk_json_encstr_w(int64_t s);
int64_t tk_json_quote_w(int64_t s);
int64_t tk_json_stringify_w(int64_t v);
int64_t tk_json_encode_w(int64_t v);
int64_t tk_json_dec_w(int64_t s);
int64_t tk_json_str_w(int64_t o, int64_t k);
int64_t tk_json_getstr_w(int64_t o, int64_t k);
int64_t tk_json_getstring_w(int64_t o, int64_t k);
int64_t tk_json_getopt_w(int64_t o, int64_t k);
int64_t tk_json_trystr_w(int64_t o, int64_t k);
int64_t tk_json_keys_w(int64_t o);
int64_t tk_json_values_w(int64_t o);
int64_t tk_json_entries_w(int64_t o);
int64_t tk_json_parsearray_w(int64_t s);
int64_t tk_json_getval_w(int64_t o, int64_t k);
int64_t tk_json_strarr_w(int64_t a);
int64_t tk_json_serialize_w(int64_t o);
int64_t tk_json_encnum_w(int64_t v);
int64_t tk_json_encbool_w(int64_t b);
int64_t tk_json_encf64_w(int64_t bits);
int64_t tk_json_encarr_w(int64_t h, int64_t depth, int64_t kind);
int64_t tk_json_encval_w(int64_t v, int64_t desc);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.60s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
#define SD(s) I(tks_strdup(s))
static int64_t alen(int64_t a) { return ((int64_t *)(intptr_t)a)[-1]; }
static int64_t aget(int64_t a, int64_t i) { return ((int64_t *)(intptr_t)a)[i]; }
static int all_sealed(int64_t a, int64_t n, const char *const *want)
{
    if (!a || alen(a) != n) { printf("        (len %lld)\n", a ? (long long)alen(a) : -1LL); return 0; }
    int ok = 1;
    for (int64_t i = 0; i < n; i++) ok &= sealed_eq(P(aget(a, i)), want[i]);
    return ok;
}
static const char DOC[] = "{\"a\":\"x\\ny\",\"n\":3,\"o\":{\"t\":[1,2]},\"k\\u0041\":\"v\"}";
int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    int64_t o = tk_json_dec_w(SD(DOC));
    CHECK(o != 0, "tk_json_dec_w parses a sealed document");
    if (!o) return bad;

    CHECK(sealed_eq(P(tk_json_enc_w(SD("q\"r"))), "\"q\\\"r\"") &&
          sealed_eq(P(tk_json_encstr_w(I("z"))), "\"z\"") &&
          sealed_eq(P(tk_json_quote_w(I("a\tb"))), "\"a\\tb\"") &&
          sealed_eq(P(tk_json_stringify_w(I(""))), "\"\"") &&
          sealed_eq(P(tk_json_encode_w(SD("e"))), "\"e\""),
          "json.enc / encstr / quote / stringify / encode: sealed");

    CHECK(sealed_eq(P(tk_json_str_w(o, SD("a"))), "x\ny") &&
          sealed_eq(P(tk_json_getstr_w(o, I("kA"))), "v") &&
          sealed_eq(P(tk_json_getstring_w(o, I("a"))), "x\ny") &&
          sealed_eq(P(tk_json_getopt_w(o, SD("kA"))), "v") &&
          sealed_eq(P(tk_json_trystr_w(o, I("a"))), "x\ny"),
          "json.str / getstr / getstring / getopt / trystr: decoded value sealed");

    static const char *const KS[] = { "a", "n", "o", "kA" };
    static const char *const ES[] = { "a=\"x\\ny\"", "n=3", "o={\"t\":[1,2]}", "kA=\"v\"" };
    static const char *const VS[] = { "x\ny", NULL, NULL, "v" };
    int64_t vals = tk_json_values_w(o);
    int vok = vals && alen(vals) == 4 && aget(vals, 1) == 0 && aget(vals, 2) == 0 &&
              sealed_eq(P(aget(vals, 0)), VS[0]) && sealed_eq(P(aget(vals, 3)), VS[3]);
    CHECK(all_sealed(tk_json_keys_w(o), 4, KS) && all_sealed(tk_json_entries_w(o), 4, ES) && vok,
          "json.keys / entries / values: every element sealed");

    static const char *const PA[] = { "1", "\"s\"", "{\"k\":[2]}" };
    CHECK(all_sealed(tk_json_parsearray_w(SD("[1, \"s\", {\"k\":[2]}]")), 3, PA),
          "json.parsearray: every element's raw text sealed");

    CHECK(sealed_eq(P(tk_json_getval_w(o, SD("o.t"))), "[1,2]") &&
          sealed_eq(P(tk_json_getval_w(o, I("n"))), "3"),
          "json.getval: the value's raw text sealed (sealed and literal path)");

    int64_t sa = tk_arr_alloc(2, 2);
    ((int64_t *)(intptr_t)sa)[0] = SD("a");
    ((int64_t *)(intptr_t)sa)[1] = I("b\"");
    CHECK(sealed_eq(P(tk_json_strarr_w(sa)), "[\"a\",\"b\\\"\"]"),
          "json.strarr: the array text sealed");

    double f = 1.5; int64_t fb; memcpy(&fb, &f, sizeof fb);
    CHECK(sealed_eq(P(tk_json_encnum_w(-9223372036854775807LL - 1)), "-9223372036854775808") &&
          sealed_eq(P(tk_json_encbool_w(1)), "true") &&
          sealed_eq(P(tk_json_encbool_w(0)), "false") &&
          sealed_eq(P(tk_json_encf64_w(fb)), "1.5"),
          "json.encnum / encbool / encf64: sealed");

    int64_t ia = tk_arr_alloc(3, 3);
    for (int i = 0; i < 3; i++) ((int64_t *)(intptr_t)ia)[i] = i + 1;
    int64_t *rec = malloc(2 * sizeof(int64_t));
    rec[0] = 7; rec[1] = SD("h\"i");
    CHECK(sealed_eq(P(tk_json_encarr_w(ia, 1, 0)), "[1,2,3]") &&
          sealed_eq(P(tk_json_encarr_w(0, 1, 0)), "null") &&
          sealed_eq(P(tk_json_encarr_w(sa, 1, 1)), "[\"a\",\"b\\\"\"]") &&
          sealed_eq(P(tk_json_encval_w(I(rec), I("{a:i,s:s}"))), "{\"a\":7,\"s\":\"h\\\"i\"}") &&
          sealed_eq(P(tk_json_encval_w(0, I("{x:i}"))), "null"),
          "json encarr / encval (j.enc of an array, a record, nil): sealed");

    const char *ser = P(tk_json_serialize_w(o));
    CHECK(ser == ((Json *)(intptr_t)o)->raw && strcmp(ser, DOC) == 0,
          "json.serialize: the document's own text, not a copy (left unsealed by design)");

    /* json.c's readers on sealed inputs answer as on unsealed ones. */
    Json js = { tks_strdup(DOC) }, ju = { DOC };
    JsonResult a1 = json_at(js, tks_strdup("o.t")), a2 = json_at(ju, "o.t");
    JsonResult m1 = json_member(js, tks_strdup("kA")), m2 = json_member(ju, "kA");
    JsonResult s1 = json_setmember(js, tks_strdup("n"), tks_strdup("4"));
    JsonResult s2 = json_setmember(ju, "n", "4");
    Json small = { tks_strdup("{\"z\":1}") };
    JsonResult g1 = json_merge(js, small), g2 = json_merge(ju, (Json){ "{\"z\":1}" });
    const char *pk[] = { tks_strdup("k"), "q" }, *pv[] = { tks_strdup("v"), "w\"" };
    const char *uk[] = { "k", "q" }, *uv[] = { "v", "w\"" };
    Json p1 = json_from_pairs(pk, pv, 2), p2 = json_from_pairs(uk, uv, 2);
    StrJsonResult r1 = json_pretty(js), r2 = json_pretty(ju);
    int same = !a1.is_err && !a2.is_err && strcmp(a1.ok.raw, a2.ok.raw) == 0 &&
               !m1.is_err && !m2.is_err && strcmp(m1.ok.raw, m2.ok.raw) == 0 &&
               !s1.is_err && !s2.is_err && strcmp(s1.ok.raw, s2.ok.raw) == 0 &&
               !g1.is_err && !g2.is_err && strcmp(g1.ok.raw, g2.ok.raw) == 0 &&
               p1.raw && p2.raw && strcmp(p1.raw, p2.raw) == 0 &&
               !r1.is_err && !r2.is_err && strcmp(r1.ok, r2.ok) == 0 &&
               json_has(js, tks_strdup("kA")) && json_is_value(tks_strdup("[1]"));
    if (!same) printf("        (sealed and unsealed inputs answered differently)\n");
    CHECK(same, "json.c readers: sealed document/key/path/pairs read as unsealed ones");
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects json -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 perl -e 'alarm 60; exec @ARGV' ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -60
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness died (rc=${rc}): $(grep -m1 -E 'ERROR|runtime error|abort' seal.out)"
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"a\":\"x\\ny\",\"n\":3,\"o\":{\"t\":[1,2]},\"k\\u0041\":\"v\"}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let a=mt json.str(o;"a") { $ok:v v; $jsonerr:e "ERR" };
  let ks=mt json.keys(o) { $ok:k k; $jsonerr:e @("ERR") };
  let es=mt json.entries(o) { $ok:k k; $jsonerr:e @("ERR") };
  let gv=json.getval(o;"o.t");
  let e1=json.enc("q\"r");
  let sa=json.strarr(@("a";"b\""));
  io.println("a=\(a.len) k3=\(ks.get(3)) \(ks.get(3).len) e2=\(es.get(2)) \(es.get(2).len) gv=\(gv) \(gv.len) e1=\(e1) \(e1.len) sa=\(sa) \(sa.len) ser=\(json.serialize(o).len)");
  io.println("enc7=\(json.enc(7)) encb=\(json.enc(true)) encf=\(json.enc(1.5)) encarr=\(json.enc(@(1;2;3))) \(json.enc(@(1;2;3)).len)");
  <0
};
TOKE
WANT1='a=3 k3=kA 2 e2=o={"t":[1,2]} 13 gv=[1,2] 5 e1="q\"r" 6 sa=["a","b\""] 11 ser=48'
WANT2='enc7=7 encb=true encf=1.5 encarr=[1,2,3] 7'
if ! "${TKC}" --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 perl -e 'alarm 30; exec @ARGV' ./p > p.out 2>&1; rc=$?
    if [ "${rc}" -eq 0 ] && grep -a -q -x -F -- "${WANT1}" p.out && grep -a -q -x -F -- "${WANT2}" p.out; then
        pass "toke: json.dec -> str/keys/entries/getval, enc of str/i64/bool/f64/array, strarr, serialize, .len under TK_STR_VERIFY=1"
    else
        fail "toke (rc=${rc}): '$(head -3 p.out | tr '\n' '|')'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1600: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
