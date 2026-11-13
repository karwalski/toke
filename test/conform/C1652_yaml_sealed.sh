#!/usr/bin/env bash
# C1652_yaml_sealed.sh -- std.yaml hands toke sealed strings, and yaml.c
# reads with tk_strlen (story 127.295, S1 step 5 tranche 14; runtime-abi
# §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen.  yaml.c's four str producers
# (yaml_enc, yaml_str, yaml_from_json, yaml_to_json) now seal at the source:
# no C caller frees what they return (they have literal fallbacks, so a free
# was never allowed), and the glue hands them to toke as they are.
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen):
#        - yaml_enc of a plain value, a value that needs quoting and "": sealed,
#          equal to hello / "key: value" (quoted) / "" (quoted);
#        - yaml_str of a key: sealed, the value;
#        - yaml_from_json of a nested object: sealed, the leading newline
#          trimmed, the expected text;
#        - yaml_to_json of a mapping and of a sequence: sealed, the JSON;
#        - through the glue the compiler calls (tk_yaml_enc_w, tk_yaml_str_w,
#          tk_yaml_fromjson_w, tk_yaml_tojson_w, tk_yaml_getstring_w): sealed;
#        - the readers, every argument sealed: yaml_str with a sealed key over
#          a sealed document and yaml_enc of a sealed value give exactly the
#          results the same calls give on unsealed copies;
#        - yaml_str of a missing key is still an error.
#   2. toke, under TK_STR_VERIFY=1: yaml.enc, yaml.str, yaml.fromjson,
#      yaml.tojson and their .len.
#
# Negative control: C1652_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base e810734c's src/stdlib + stdlib: 3 passed, 5 failed -- the readers,
#   missing-key and toke cases pass there (an unsealed string read with
#   strlen was always correct); every producer case reports "not sealed".
# Measured: this branch 8 passed (7 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1652_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1652_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1652: std.yaml results sealed; yaml.c reads with tk_strlen (127.295)"
echo "---------------------------------------------------------------------"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "yaml.h"
#include "capabilities.h"
#include "tk_runtime.h"
int64_t tk_yaml_enc_w(int64_t v);
int64_t tk_yaml_dec_w(int64_t s);
int64_t tk_yaml_str_w(int64_t v, int64_t key);
int64_t tk_yaml_fromjson_w(int64_t json);
int64_t tk_yaml_tojson_w(int64_t v);
int64_t tk_yaml_getstring_w(int64_t m, int64_t key);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.60s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}
static const char *DOC = "name: toke\ncount: 42\nmsg: \"hi there\"\n";
int main(void)
{
    char a0[] = "seal", a1[] = "--allow-all";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */

    CHECK(sealed_eq(yaml_enc("hello"), "hello")
          && sealed_eq(yaml_enc("key: value"), "\"key: value\"")
          && sealed_eq(yaml_enc(""), "\"\""),
          "yaml_enc: plain, quoted and empty values sealed");

    YamlResult d = yaml_dec(DOC);
    StrYamlResult r = yaml_str(d.ok, "name");
    CHECK(!d.is_err && !r.is_err && sealed_eq(r.ok, "toke"), "yaml_str: the value sealed");

    CHECK(sealed_eq(yaml_from_json("{\"a\":{\"b\":7},\"c\":\"x\"}"), "a: \n  b: 7\nc: x"),
          "yaml_from_json: sealed, leading newline trimmed");

    CHECK(sealed_eq(yaml_to_json("k: v\nn: 3\n"), "{\"k\":\"v\",\"n\":3}")
          && sealed_eq(yaml_to_json("- a\n- 2\n"), "[\"a\",2]"),
          "yaml_to_json: mapping and sequence sealed");

    int64_t h = tk_yaml_dec_w(I(DOC));
    CHECK(sealed_eq(P(tk_yaml_enc_w(I("a#b"))), "\"a#b\"")
          && sealed_eq(P(tk_yaml_str_w(h, I("count"))), "42")
          && sealed_eq(P(tk_yaml_getstring_w(h, I("name"))), "toke")
          && sealed_eq(P(tk_yaml_fromjson_w(I("{\"z\":1}"))), "z: 1")
          && sealed_eq(P(tk_yaml_tojson_w(I("z: 1\n"))), "{\"z\":1}"),
          "glue: enc, str, getstring, fromjson, tojson sealed");

    /* Readers over sealed arguments give the same results as unsealed. */
    const char *sdoc = tks_strdup(DOC), *skey = tks_strdup("msg");
    const char *sval = tks_strdup("a: b");
    int ok = tks_hdrlen(sdoc) >= 0 && tks_hdrlen(skey) >= 0 && tks_hdrlen(sval) >= 0;
    StrYamlResult rs = yaml_str(yaml_dec(sdoc).ok, skey);
    StrYamlResult ru = yaml_str(yaml_dec(DOC).ok, "msg");
    ok = ok && !rs.is_err && !ru.is_err && strcmp(rs.ok, ru.ok) == 0;
    ok = ok && strcmp(yaml_enc(sval), yaml_enc("a: b")) == 0;
    CHECK(ok, "readers: sealed document, key and value read as unsealed ones");

    /* yaml_str of a missing key still fails. */
    CHECK(yaml_str(d.ok, "nope").is_err, "yaml_str of a missing key is an error");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects yaml -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=yaml:std.yaml;
f=main():i64{
  let e=yaml.enc("key: value");
  io.println("enc=\(e) \(e.len)");
  mt yaml.dec("name: toke\nn: 3\n") {
    $ok:y mt yaml.str(y;"name") {$ok:s io.println("str=\(s) \(s.len)"); $err:x io.println("str failed")};
    $err:x io.println("dec failed")
  };
  let fj=yaml.fromjson("{\"a\":1,\"b\":\"two\"}");
  io.println("fj=\(fj.len)");
  let tj=yaml.tojson("a: 1\nb: two\n");
  io.println("tj=\(tj) \(tj.len)");
  <0
};
TOKE
WANT='enc="key: value" 12
str=toke 4
fj=11
tj={"a":1,"b":"two"} 17'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: yaml.enc/str/fromjson/tojson and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "---------------------------------------------------------------------"
echo "C1652: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
