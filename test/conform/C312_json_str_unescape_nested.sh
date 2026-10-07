#!/usr/bin/env bash
# C312_json_str_unescape_nested.sh — json.str decodes JSON escapes, and a
# nested value has a typed read (story 138.12; loke T-44, T-47a, T-32).
#
# THE DEFECTS.
#   T-44/T-47a. json_str copied the byte after every backslash: "\n" came
#     back as the letter n, "\t" as t, "A" as the five bytes u0041.  Only
#     \" and \\ were right, by accident.  json_keys had the same loop.
#   T-32. std.json's typed accessors read one key of a flat object and
#     nothing typed reached a nested value; the tempting workaround, the
#     untyped json.getval, returns the value's raw TEXT, which the checker let
#     into json.i64's Json parameter, and json.i64 segfaulted on it.
#
# THE FIX.  json_str and json_keys decode through strbuf_append_json_unescape
# (the one copy of the rule, 127.229).  json.at(Json;str) -> Json!JsonErr walks
# a dotted path ("usage.input_tokens", "r.0.id").  json.getval is declared
# (Json;str) -> str and returns "" for a missing path instead of a null.
# (That a str is still accepted where a Json is declared is the checker's --
# reported separately; this suite does not depend on it.)
#
# WHAT IS ASSERTED.
#   Part 1 (C driver, json.c under ASan+UBSan): every escape json_str can
#     meet, a surrogate pair, an escaped key via json_keys, json_at into an
#     object, an array and a missing key.
#   Part 2 (toke programs): T-44's and T-47a's bytes; T-32's value through
#     json.at + json.i64; json.at on a missing path takes $jsonerr;
#     json.getval of a missing path is "".
#
# Negative control: C312_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-09-27 against a copy of this tree with json_str's old
# copy-the-next-byte loop restored: 6 of 13 fail -- every escape case in
# both parts; the json_at cases, which that loop does not touch, pass.
#
# Story: 138.12
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C312_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c312_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C312: json.str unescape, json.at nested read (138.12)"
echo "------------------------------------------------------"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "json.h"
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
static int str_is(Json j, const char *k, const char *want) {
    StrJsonResult r = json_str(j, k);
    int ok = !r.is_err && strcmp(r.ok, want) == 0;
    if (!ok) printf("        got [%s]\n", r.is_err ? "(err)" : r.ok);
    if (!r.is_err) free((void *)r.ok);
    return ok;
}
int main(void) {
    Json j = { "{\"n\":\"a\\nb\",\"t\":\"a\\tb\",\"r\":\"a\\rb\",\"b\":\"a\\bb\",\"f\":\"a\\fb\","
               "\"q\":\"a\\\"b\",\"bs\":\"a\\\\b\",\"sl\":\"a\\/b\",\"u\":\"\\u0041\\u00e9\","
               "\"sp\":\"\\ud83d\\ude00\",\"plain\":\"x\",\"e\":\"\",\"k\\u0041y\":1}" };
    CHECK(str_is(j, "n", "a\nb"), "\\n is a newline (T-44)");
    CHECK(str_is(j, "t", "a\tb") && str_is(j, "r", "a\rb") && str_is(j, "b", "a\bb") &&
          str_is(j, "f", "a\fb"), "\\t \\r \\b \\f are their control bytes (T-47a)");
    CHECK(str_is(j, "q", "a\"b") && str_is(j, "bs", "a\\b") && str_is(j, "sl", "a/b"),
          "\\\" \\\\ \\/ are their byte");
    CHECK(str_is(j, "u", "A\xc3\xa9"), "\\u0041\\u00e9 is A and UTF-8 e-acute (T-47a)");
    CHECK(str_is(j, "sp", "\xf0\x9f\x98\x80"), "a surrogate pair is one 4-byte code point");
    CHECK(str_is(j, "plain", "x") && str_is(j, "e", ""), "plain and empty strings unchanged");
    StrArrayJsonResult ks = json_keys(j);
    int found = 0;
    if (!ks.is_err) for (uint64_t i = 0; i < ks.ok.len; i++) if (!strcmp(ks.ok.data[i], "kAy")) found = 1;
    CHECK(found, "json_keys decodes an escaped key");

    Json d = { "{\"model\":\"abc\",\"usage\":{\"input_tokens\":150},\"r\":[{\"id\":7},{\"id\":8}]}" };
    JsonResult u = json_at(d, "usage");
    I64JsonResult n = u.is_err ? (I64JsonResult){0} : json_i64(u.ok, "input_tokens");
    CHECK(!u.is_err && !n.is_err && n.ok == 150, "json_at(usage) then json_i64 is 150 (T-32)");
    JsonResult r1 = json_at(d, "r.1");
    I64JsonResult id = r1.is_err ? (I64JsonResult){0} : json_i64(r1.ok, "id");
    CHECK(!r1.is_err && !id.is_err && id.ok == 8, "json_at(r.1) indexes the array");
    CHECK(json_at(d, "usage.nope").is_err, "json_at of a missing key is an error");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o t t.c "${S}/json.c" > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL\|^        got' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing:"
        grep -a 'ERROR: AddressSanitizer\|runtime error' t.out | head -3 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

# ── Part 2: end to end ───────────────────────────────────────────────────
mkdir e2e && cd e2e
run_tk() {   # name expected-output
    local name=$1 want=$2 got
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" --out "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        echo "  FAIL: ${name}.tk did not compile"; sed 's/^/        /' "${name}.log" | head -4; FAIL=$((FAIL + 1)); return
    fi
    got="$(./"${name}" 2>&1)"
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: ${name}: ${want}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${want}', got '${got}'"; FAIL=$((FAIL + 1))
    fi
}
cat > t44.tk <<'TOKE'
m=t44;i=io:std.io;i=str:std.str;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"content\":\"summarisation\\n\"}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let s=mt json.str(o;"content") { $ok:x x; $jsonerr:e "ERR" };
  let n=str.len(s);
  io.println("len=\(n) lastbyte=\(str.charat(s;n-1))");
  <0
};
TOKE
run_tk t44 "len=14 lastbyte=10"
cat > t47.tk <<'TOKE'
m=t47a;i=io:std.io;i=str:std.str;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"c\":\"a\\nb\\tc\\rd\\\"e\\\\f\\u0041\"}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let s=mt json.str(o;"c") { $ok:x x; $jsonerr:e "ERR" };
  let n=str.len(s);
  let out=mut."";
  lp(let k=0;k<n;k=k+1){
    out=str.concat(out;str.concat(str.fromint(str.charat(s;k));" "))
  };
  io.println("bytes=\(out)");
  <0
};
TOKE
run_tk t47 "bytes=97 10 98 9 99 13 100 34 101 92 102 65 "
cat > t32.tk <<'TOKE'
m=t32;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"model\":\"abc\",\"usage\":{\"input_tokens\":150}}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let u=mt json.at(o;"usage") { $ok:j j; $jsonerr:e o };
  let tokens=mt json.i64(u;"input_tokens") { $ok:v v; $jsonerr:e 0-1 };
  let miss:str=mt json.at(o;"usage.nope") { $ok:j "ok"; $jsonerr:e "err" };
  let gv=json.getval(o;"nope");
  io.println("tokens=\(tokens) miss=\(miss) getval-missing=[\(gv)]");
  <0
};
TOKE
run_tk t32 "tokens=150 miss=err getval-missing=[]"

echo "------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
