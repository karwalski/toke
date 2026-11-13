#!/usr/bin/env bash
# C1541_soap_sealed.sh -- std.soap hands toke sealed strings, and soap.c
# reads its inputs with tk_strlen (story 127.295, S1 step 5 tranche 10;
# runtime-abi §3.1).
#
# A sealed string carries its byte length in a header that tk_strlen reads in
# O(1); the header must always equal strlen, and a sealed string is never
# freed (free() of one aborts: the address did not come from malloc).
#
# WHAT IS ASSERTED:
#   1. C (ASan+UBSan, run under TK_STR_VERIFY=1, so every trusted length read
#      asserts header == strlen), each result sealed with the exact bytes:
#        - soap_envelope / soap12_envelope, with and without a header, from
#          sealed inputs;
#        - soap_fault with and without detail (escaped);
#        - soap_extract_body (soap:, SOAP-ENV: and unprefixed Body; no Body
#          -> sealed ""; NULL -> sealed "") and soap_extract_fault (found ->
#          sealed; none -> NULL);
#        - soap_wsse_username_token / soap_wsse_timestamp (escaped);
#        - the glue tk_soap_*_w, and tk_soap_extractfault_w's "" for none.
#   2. toke, under TK_STR_VERIFY=1: envelope -> extractbody round trip,
#      extractfault of a fault envelope, and `.len` of each.
#
# Negative control: C1541_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base d530faaa's src/stdlib + stdlib: 2 passed, 6 failed -- every
#   producer case reports "not sealed"; the toke cases pass there, because
#   an unsealed string read with strlen was always correct.
# Measured: this branch 8 passed (6 C + 2 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1541_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1541_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1541: std.soap results sealed; soap.c reads with tk_strlen (127.295)"
echo "--------------------------------------------------------------------------------"

# ── 1. C ─────────────────────────────────────────────────────────────────────
cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "soap.h"
int64_t tk_soap_envelope_w(int64_t body, int64_t header);
int64_t tk_soap_fault_w(int64_t code, int64_t string, int64_t detail);
int64_t tk_soap_extractbody_w(int64_t envelope);
int64_t tk_soap_extractfault_w(int64_t envelope);
int64_t tk_soap_wsseusernametoken_w(int64_t user, int64_t pass);
int64_t tk_soap_wssetimestamp_w(int64_t created, int64_t expires);
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
#define XML "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
#define NS11 "http://schemas.xmlsoap.org/soap/envelope/"
#define NS12 "http://www.w3.org/2003/05/soap-envelope"
int main(void)
{
    CHECK(sealed_eq(soap_envelope(tks_strdup("<a>1</a>"), NULL),
                    XML "<soap:Envelope xmlns:soap=\"" NS11 "\">\n  <soap:Body><a>1</a></soap:Body>\n</soap:Envelope>") &&
          sealed_eq(soap12_envelope("<b/>", tks_strdup("<h/>")),
                    XML "<soap:Envelope xmlns:soap=\"" NS12 "\">\n  <soap:Header><h/></soap:Header>\n"
                    "  <soap:Body><b/></soap:Body>\n</soap:Envelope>"),
          "soap_envelope / soap12_envelope (with and without header): sealed");
    CHECK(sealed_eq(soap_fault(tks_strdup("Client"), "a<b", "x&y"),
                    "<soap:Fault>\n  <faultcode>Client</faultcode>\n  <faultstring>a&lt;b</faultstring>\n"
                    "  <detail>x&amp;y</detail>\n</soap:Fault>") &&
          sealed_eq(soap_fault("S", "E", ""),
                    "<soap:Fault>\n  <faultcode>S</faultcode>\n  <faultstring>E</faultstring>\n</soap:Fault>"),
          "soap_fault with and without detail: sealed, escaped");
    CHECK(sealed_eq(soap_extract_body(tks_strdup("<soap:Envelope><soap:Body x=\"1\"><q>v</q></soap:Body></soap:Envelope>")), "<q>v</q>") &&
          sealed_eq(soap_extract_body("<SOAP-ENV:Body>s</SOAP-ENV:Body>"), "s") &&
          sealed_eq(soap_extract_body("<Body>u</Body>"), "u") &&
          sealed_eq(soap_extract_body("<none/>"), "") &&
          sealed_eq(soap_extract_body(NULL), ""),
          "soap_extract_body (three prefixes, none, NULL): sealed");
    CHECK(sealed_eq(soap_extract_fault(tks_strdup("<x><soap:Fault><c/></soap:Fault></x>")), "<soap:Fault><c/></soap:Fault>") &&
          sealed_eq(soap_extract_fault("<Fault>f</Fault>"), "<Fault>f</Fault>") &&
          soap_extract_fault("<soap:Body/>") == NULL,
          "soap_extract_fault: found -> sealed, none -> NULL");
    CHECK(sealed_eq(soap_wsse_username_token(tks_strdup("u&"), "p<"), NULL) &&
          strstr(soap_wsse_username_token("u&", "p<"), "<wsse:Username>u&amp;</wsse:Username>") &&
          sealed_eq(soap_wsse_timestamp("2024-01-01T00:00:00Z", tks_strdup("'e'")), NULL) &&
          strstr(soap_wsse_timestamp("c", "'e'"), "<wsu:Expires>&apos;e&apos;</wsu:Expires>"),
          "soap_wsse_username_token / soap_wsse_timestamp: sealed, escaped");
    const char *env = P(tk_soap_envelope_w(I("<k/>"), 0));
    CHECK(sealed_eq(env, NULL) &&
          sealed_eq(P(tk_soap_extractbody_w(I(env))), "<k/>") &&
          sealed_eq(P(tk_soap_fault_w(I("c"), I("s"), 0)), NULL) &&
          strcmp(P(tk_soap_extractfault_w(I(env))), "") == 0 &&
          sealed_eq(P(tk_soap_extractfault_w(I("<Fault/>x</Fault>"))), "<Fault/>x</Fault>") &&
          sealed_eq(P(tk_soap_wsseusernametoken_w(I("a"), I("b"))), NULL) &&
          sealed_eq(P(tk_soap_wssetimestamp_w(I("c"), I("d"))), NULL),
          "soap glue (envelope, extractbody, fault, extractfault, wsse): sealed");
    return bad;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list (C696, C703 case 12).
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects soap -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
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
i=soap:std.soap;
f=main():i64{
  let env=soap.envelope("<Q>v</Q>";"<H/>");
  let b=soap.extractbody(env);
  io.println("body=[\(b)] \(b.len) \(env.len)");
  let ft=soap.extractfault(soap.envelope(soap.fault("Server";"E&";"d");""));
  io.println("fault=\(ft.len)");
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 ./p > p.out 2>&1; rc=$?
    if [ "${rc}" -eq 0 ] && grep -a -q -x -F -- "body=[<Q>v</Q>] 8 194" p.out; then
        pass "toke: soap.envelope -> soap.extractbody round trip and .len under TK_STR_VERIFY=1"
    else
        fail "toke: envelope/extractbody (rc=${rc}): '$(grep -a '^body=' p.out | head -1)'"
    fi
    if grep -a -q -x -F -- "fault=115" p.out; then
        pass "toke: soap.extractfault of a fault envelope"
    else
        fail "toke: extractfault: '$(grep -a '^fault=' p.out | head -1)'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1541: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
