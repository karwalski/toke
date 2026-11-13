#!/usr/bin/env bash
# C1510_i18n_env_sse_sealed.sh -- std.i18n hands out sealed strings,
# env.expand is sealed, and std.i18n / std.env / std.sse read their inputs
# with tk_strlen (story 127.295, S1 step 5 tranche 9; runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen):
#        - i18n_get (TOON, YAML and JSON bundles, and the missing-key
#          fallback), i18n_fmt (substitution and fallback) and i18n_locale
#          return sealed strings with the exact expected bytes, and so do the
#          glue entry points tk_i18n_get_w / tk_i18n_fmt_w / tk_i18n_locale_w;
#        - env_expand and tk_env_expand_w return sealed strings;
#        - env_get / env_get_or are NOT sealed: env.tki declares them
#          "ownership": "caller" (121.78), so they stay heap copies the
#          harness frees under ASan;
#        - sse_emit / sse_emitdata, given sealed inputs (event, id, and a
#          three-line data block), produce the exact wire bytes; the event
#          stays malloc'd (sse_glue.c writes and frees each one) and is
#          freed here under ASan.
#   2. toke, under TK_STR_VERIFY=1: i18n.get / i18n.fmt / i18n.locale give
#      the expected text and `.len`; env.getor/env.get read; sse.emit and
#      sse.emitdata write the exact wire format to stdout; exit 0.
#
# Negative control: C1510_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   main e31aac45's src/stdlib + stdlib: 10 passed, 6 failed -- the i18n and
#   env_expand producer/glue strings carry no header ("not sealed"); the
#   env_get, sse and toke cases pass there, because an unsealed string read
#   with strlen was always correct.
# Measured: this branch 16 passed (10 C + 6 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1510_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1510_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1510: std.i18n / env.expand sealed; i18n, env, sse read with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

mkdir -p lang
printf 'strings[2]{key,value}:\nhello|Hello World\nmsg|Hi {user}, {count} items\n' > lang/t.en.toon
printf 'greeting: "Bonjour"\nwelcome: Salut {name}\n' > lang/y.fr.yaml
printf '{"greeting":"Hallo","n":42}' > lang/j.de.json

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "i18n.h"
#include "env.h"
#include "sse.h"
int64_t tk_i18n_get_w(int64_t bundle, int64_t key);
int64_t tk_i18n_fmt_w(int64_t bundle, int64_t key, int64_t args);
int64_t tk_i18n_locale_w(void);
int64_t tk_env_expand_w(int64_t tmpl);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : ".";
    char base[1024];

    /* i18n: three bundle formats, sealed keys/args as toke passes them */
    snprintf(base, sizeof base, "%s/lang/t", dir);
    I18nBundleResult t = i18n_load(base, "en");
    snprintf(base, sizeof base, "%s/lang/y", dir);
    I18nBundleResult y = i18n_load(base, "fr");
    snprintf(base, sizeof base, "%s/lang/j", dir);
    I18nBundleResult j = i18n_load(base, "de");
    CHECK(!t.is_err && !y.is_err && !j.is_err &&
          sealed_eq(i18n_get(t.ok, tks_strdup("hello")), "Hello World") &&
          sealed_eq(i18n_get(y.ok, "greeting"), "Bonjour") &&
          sealed_eq(i18n_get(j.ok, "greeting"), "Hallo") &&
          sealed_eq(i18n_get(j.ok, "n"), "42"),
          "i18n_get: TOON, YAML (quoted), JSON (string and number) values sealed");
    CHECK(sealed_eq(i18n_get(t.ok, tks_strdup("no.such")), "no.such"),
          "i18n_get missing key: sealed copy of the key");
    CHECK(sealed_eq(i18n_fmt(t.ok, "msg", tks_strdup("user=Ann|count=3")), "Hi Ann, 3 items") &&
          sealed_eq(i18n_fmt(y.ok, "welcome", "name=Luc"), "Salut Luc") &&
          sealed_eq(i18n_fmt(t.ok, "absent", "x=1"), "absent"),
          "i18n_fmt: substitution and missing-key fallback sealed");
    CHECK(sealed_eq(i18n_locale(), NULL), "i18n_locale: sealed");
    CHECK(sealed_eq(P(tk_i18n_get_w(I(&t.ok), I("hello"))), "Hello World") &&
          sealed_eq(P(tk_i18n_fmt_w(I(&t.ok), I("msg"), I("user=B|count=9"))), "Hi B, 9 items") &&
          sealed_eq(P(tk_i18n_locale_w()), NULL),
          "i18n glue (get, fmt, locale): sealed");

    /* env */
    setenv("C1510_V", "vee", 1);
    CHECK(sealed_eq(env_expand(tks_strdup("a=$C1510_V ${C1510_V}$$")), "a=vee vee$") &&
          sealed_eq(P(tk_env_expand_w(I("[${C1510_V}]"))), "[vee]"),
          "env_expand / tk_env_expand_w: sealed");
    EnvGetResult g = env_get(tks_strdup("C1510_V"));
    char *go = (char *)env_get_or("C1510_UNSET_XYZ", tks_strdup("dflt"));
    CHECK(!g.is_err && strcmp(g.ok, "vee") == 0 && tks_hdrlen(g.ok) < 0 &&
          strcmp(go, "dflt") == 0 && tks_hdrlen(go) < 0,
          "env_get / env_get_or: caller-owned heap copies, not sealed, free()d");
    free((void *)g.ok); free(go);

    /* sse: sealed inputs, malloc'd events */
    TkSseCtx *ctx = sse_new();
    TkSseEvent ev = { tks_strdup("upd"), tks_strdup("one\ntwo\nthree"), tks_strdup("7"), 50 };
    char *e1 = (char *)sse_emit(ctx, ev);
    CHECK(e1 && strcmp(e1, "event: upd\ndata: one\ndata: two\ndata: three\nid: 7\nretry: 50\n\n") == 0 &&
          tks_hdrlen(e1) < 0, "sse_emit: exact wire bytes from sealed inputs; malloc'd, free()d");
    free(e1);
    char *e2 = (char *)sse_emitdata(ctx, tks_strdup("tail\n"));
    char *e3 = (char *)sse_emitdata(ctx, tks_strdup(""));
    CHECK(e2 && strcmp(e2, "data: tail\ndata: \n\n") == 0 && tks_hdrlen(e2) < 0 &&
          e3 && strcmp(e3, "data: \n\n") == 0,
          "sse_emitdata: trailing newline and empty data; malloc'd, free()d");
    free(e2); free(e3);
    char *ka = (char *)sse_keepalive(ctx);
    CHECK(ka && strcmp(ka, ": keepalive\n\n") == 0, "sse_keepalive: comment, free()d");
    free(ka);
    sse_free(ctx);
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects i18n env sse -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal "${WORK}" > seal.out 2>&1; rc=$?
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
i=i18n:std.i18n;
i=env:std.env;
i=sse:std.sse;
f=main():i64{
  let ui=mt i18n.load("lang/t";"en") {$ok:u u;$err:e $i18nbundle{data:""}};
  let h=i18n.get(ui;"hello");
  io.println("get=[\(h)] \(h.len)");
  let m=i18n.fmt(ui;"msg";"user=Zoe|count=12");
  io.println("fmt=[\(m)] \(m.len)");
  let l=i18n.locale();
  io.println("locale=\(l.len>0)");
  let s=env.set("C1510_P";"pval");
  let g=mt env.get("C1510_P") {$ok:v v;$err:e "ERR"};
  let d=env.getor("C1510_UNSET_XYZ";"dflt");
  io.println("env=[\(g)] \(g.len) [\(d)] \(d.len)");
  let ctx=$ssectx{id:0;open:true};
  let ev=$sseevent{id:"9";event:"tick";data:"a\nb";retry:0};
  mt sse.emit(ctx;ev) {$ok:v io.println("emit-ok");$err:e io.println("emit-err")};
  mt sse.emitdata(ctx;"z") {$ok:v io.println("emitdata-ok");$err:e io.println("emitdata-err")};
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p --allow-read --allow-env > p.out 2>&1; rc=$?
    if [ "${rc}" -eq 0 ]; then pass "toke program exits 0 under TK_STR_VERIFY=1"
    else fail "toke program exited ${rc}: $(head -c 300 p.out)"; fi
    for want in "get=[Hello World] 11" "fmt=[Hi Zoe, 12 items] 16" "locale=true" \
                "env=[pval] 4 [dflt] 4"; do
        if grep -a -q -x -F -- "${want}" p.out; then
            pass "toke: ${want}"
        else
            fail "toke: expected '${want}', got '$(grep -a -F -- "${want%%=*}=" p.out | head -1)'"
        fi
    done
    # sse writes the fd directly while io.println is buffered, so the two
    # interleave in any order on a pipe: compare the wire lines on their own.
    wire="$(grep -a -v -E '^(get|fmt|locale|env)=|^emit(data)?-(ok|err)$' p.out)"
    want_wire="$(printf 'event: tick\ndata: a\ndata: b\nid: 9\n\ndata: z\n')"
    grep -a -q -x 'emit-ok' p.out && grep -a -q -x 'emitdata-ok' p.out || wire="(no \$ok arm) ${wire}"
    if [ "${wire}" = "${want_wire}" ]; then
        pass "toke: sse.emit / sse.emitdata wire format"
    else
        fail "toke: sse wire format: $(printf '%s' "${wire}" | tr '\n' '|' | head -c 200)"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1510: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
