#!/usr/bin/env bash
# C1671_toon_sealed.sh -- std.toon hands toke sealed strings, and toon.c
# reads with tk_strlen (story 127.295, S1 step 5 tranche 16; runtime-abi
# §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen.  toon.c seals at the source:
# toon_enc, toon_str, toon_from_json and toon_to_json copy their finished
# TkStrBuf once into a sealed string (measured as a C string) and free the
# buffer; their fallbacks stay the literals "" and "[]", so no C caller ever
# could free them, and none does now (C440 stopped).  toon_arr's element raws
# stay malloc'd: the caller's storage (toon.arr wraps each in a $toonval).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), over the glue the compiler calls and over
#      toon.c itself:
#        - toon.enc / stringify / tostr of "a|b": a sealed "a\|b"; of "" a
#          sealed "";
#        - toon.str and toon.getstring: the decoded value, sealed;
#        - toon.fromjson / toon.tojson: sealed, equal to the expected text;
#        - the readers, every argument sealed: toon_enc, toon_dec + toon_str
#          (sealed document and key) and toon_from_json give exactly what
#          the same calls give on unsealed copies.
#   2. toke, under TK_STR_VERIFY=1: toon.enc, toon.str, toon.fromjson,
#      toon.tojson and their .len.
#
# Negative control: C1671_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base 684b452c's src/stdlib + stdlib: 2 passed, 3 failed -- the reader
#   and toke cases pass there (an unsealed string read with strlen was always
#   correct); every producer case reports "not sealed".
# Measured: this branch 5 passed (4 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1671_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1671_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1671: std.toon results sealed; toon.c reads with tk_strlen (127.295)"
echo "---------------------------------------------------------------------"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "toon.h"
int64_t tk_toon_enc_w(int64_t v);
int64_t tk_toon_stringify_w(int64_t v);
int64_t tk_toon_tostr_w(int64_t v);
int64_t tk_toon_dec_w(int64_t s);
int64_t tk_toon_str_w(int64_t v, int64_t key);
int64_t tk_toon_getstring_w(int64_t v, int64_t key);
int64_t tk_toon_fromjson_w(int64_t json);
int64_t tk_toon_tojson_w(int64_t v);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int sealed_eq(const char *s, const char *want)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.40s')\n", s); return 0; }
    if ((size_t)h != strlen(s) || (want && strcmp(s, want) != 0))
        printf("        (got '%.40s', header %lld)\n", s, (long long)h);
    return (size_t)h == strlen(s) && (!want || strcmp(s, want) == 0);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    CHECK(sealed_eq(P(tk_toon_enc_w(I("a|b"))), "a\\|b")
          && sealed_eq(P(tk_toon_stringify_w(I("x\ny"))), "x\\ny")
          && sealed_eq(P(tk_toon_tostr_w(I("plain"))), "plain")
          && sealed_eq(P(tk_toon_enc_w(I(""))), ""),
          "toon.enc / stringify / tostr: sealed, escaped; of \"\" a sealed \"\"");

    const char *doc = "{id,name}:\n7|a\\|b\n";
    int64_t t = tk_toon_dec_w(I(doc));
    CHECK(t && sealed_eq(P(tk_toon_str_w(t, I("name"))), "a|b")
          && sealed_eq(P(tk_toon_getstring_w(t, I("id"))), "7"),
          "toon.str / getstring: the decoded value, sealed");

    const char *j = "[{\"id\":1,\"n\":\"x|y\"},{\"id\":2,\"n\":\"z\"}]";
    const char *tn = P(tk_toon_fromjson_w(I(j)));
    CHECK(sealed_eq(tn, "data[2]{id,n}:\n1|x\\|y\n2|z\n")
          && sealed_eq(P(tk_toon_tojson_w(I(tn))), j),
          "toon.fromjson / toon.tojson: sealed, round trip");

    /* Readers over sealed arguments give what unsealed ones give. */
    const char *sv = tks_strdup("p|q\nr"), *sd = tks_strdup(doc), *sk = tks_strdup("name");
    const char *sj = tks_strdup(j);
    Toon td = { sd };
    StrToonResult a = toon_str(td, sk), b = toon_str((Toon){ doc }, "name");
    int r = tks_hdrlen(sv) >= 0 && tks_hdrlen(sd) >= 0
            && strcmp(toon_enc(sv), toon_enc("p|q\nr")) == 0
            && !a.is_err && !b.is_err && strcmp(a.ok, b.ok) == 0
            && strcmp(toon_from_json(sj), toon_from_json(j)) == 0
            && strcmp(toon_to_json(tks_strdup(tn)), toon_to_json("data[2]{id,n}:\n1|x\\|y\n2|z\n")) == 0;
    CHECK(r, "toon_enc / toon_str / toon_from_json / toon_to_json: sealed inputs read as unsealed ones");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects toon str -- "${CF[@]}")" \
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
    if [ "${rc}" -ne 0 ] && grep -a -q -E 'ERROR: AddressSanitizer|runtime error' seal.out; then
        fail "C harness stopped: $(grep -a -m1 -o -E 'AddressSanitizer: [a-z-]+|runtime error: .{0,60}' seal.out)"
    elif [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

# ── 2. toke ──────────────────────────────────────────────────────────────────
cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=str:std.str;
i=toon:std.toon;
f=main():i64{
  let e=toon.enc(str.concat("a|";"b"));
  io.println("enc=\(e) \(e.len)");
  let t=mt toon.dec("{id,name}:\n7|x\\|y\n"){$ok:v v;$err:e toon.empty()};
  let n=mt toon.str(t;str.concat("na";"me")){$ok:v v;$err:e "?"};
  io.println("str=\(n) \(n.len)");
  let tj=toon.fromjson("[{\"k\":\"v\"}]");
  let back=toon.tojson(tj);
  io.println("json=\(tj.len) \(back) \(back.len)");
  <0
};
TOKE
WANT='enc=a\|b 4
str=x|y 3
json=14 [{"k":"v"}] 11'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: toon.enc / str / fromjson / tojson and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "---------------------------------------------------------------------"
echo "C1671: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
