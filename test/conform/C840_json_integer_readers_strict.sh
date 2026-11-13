#!/usr/bin/env bash
# C840_json_integer_readers_strict.sh — json.u64 and json.i64 read a JSON
# integer and nothing else (story 127.303).
#
# THE DEFECTS.
#   * json.u64 used strtoull, which accepts a leading '-' and wraps:
#     json.u64 of {"n":-1} took $ok with 2^64-1.  docs/stdlib/json.md says a
#     value that is not a non-negative integer is $jsonerr.$type.
#   * json.u64 and json.i64 read a numeric prefix: 1.5 read as 1 and 1e3 as 1.
#
# THE FIX.  json_u64 requires the value to start with a digit, json_i64 with
#   a '-' or a digit, and both require strtou?ll to consume the whole value
#   (end == the value end json_locate found).  Out-of-range stays $type.
#
# WHAT IS ASSERTED.
#   Part 1 (C driver, json.c under ASan+UBSan): accept / reject per value.
#   Part 2 (toke program): the $type arm end to end, and the legal reads.
#
# Negative control: C840_STDLIB=<repo>/src/stdlib builds against another
# tree.  Measured 2026-11-09 against main 7758dcf3: 7 passed, 11 failed --
# exactly the negative / fraction / exponent cases in both parts.
#
# Story: 127.303
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C840_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c840_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C840: json.u64 / json.i64 read a whole JSON integer (127.303)"
echo "---------------------------------------------------------------------------"

# ── Part 1: the C core ───────────────────────────────────────────────────
cat > t.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include "json.h"
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
static int u_type(const char *doc) {
    U64JsonResult r = json_u64((Json){ doc }, "n");
    return r.is_err && r.err.kind == JSON_ERR_TYPE;
}
static int i_type(const char *doc) {
    I64JsonResult r = json_i64((Json){ doc }, "n");
    return r.is_err && r.err.kind == JSON_ERR_TYPE;
}
int main(void) {
    U64JsonResult u0 = json_u64((Json){ "{\"n\":0}" }, "n");
    U64JsonResult um = json_u64((Json){ "{\"n\":18446744073709551615}" }, "n");
    U64JsonResult us = json_u64((Json){ "{\"n\" : 42 , \"m\":1}" }, "n");
    CHECK(!u0.is_err && u0.ok == 0 && !um.is_err && um.ok == UINT64_MAX &&
          !us.is_err && us.ok == 42, "u64 reads 0, 2^64-1 and a spaced member");
    CHECK(u_type("{\"n\":-1}"), "u64 of -1 is Type (strtoull wrapped it)");
    CHECK(u_type("{\"n\":-0}"), "u64 of a '-'-signed value is Type");
    CHECK(u_type("{\"n\":1.5}"), "u64 of 1.5 is Type (not its prefix 1)");
    CHECK(u_type("{\"n\":1e3}"), "u64 of 1e3 is Type");
    CHECK(u_type("{\"n\":18446744073709551616}"), "u64 out of range is Type");
    CHECK(u_type("{\"n\":\"5\"}") && u_type("{\"n\":true}"), "u64 of a string / bool is Type");
    I64JsonResult i1 = json_i64((Json){ "{\"n\":-1}" }, "n");
    I64JsonResult im = json_i64((Json){ "{\"n\":-9223372036854775808}" }, "n");
    CHECK(!i1.is_err && i1.ok == -1 && !im.is_err && im.ok == INT64_MIN, "i64 reads -1 and INT64_MIN");
    CHECK(i_type("{\"n\":1.5}") && i_type("{\"n\":-2.0}"), "i64 of a fraction is Type");
    CHECK(i_type("{\"n\":1e3}"), "i64 of an exponent is Type");
    CHECK(i_type("{\"n\":9223372036854775808}"), "i64 out of range is Type");
    I64JsonResult self = json_i64((Json){ "7.5" }, "");
    CHECK(self.is_err && self.err.kind == JSON_ERR_TYPE, "i64 self-read (key \"\") of 7.5 is Type");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# 127.295: json.c reads lengths with tk_strlen, whose string-heap state lives
# in str.c, so std.json's link set comes from stdlib_link_set.sh --objects,
# never a hand list.
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINK=()
if LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --cc "${CC}" \
        --objects json -- "${CF[@]}" 2>ls.log)"; then
    while IFS= read -r l; do [ -n "$l" ] && LINK+=("$l"); done <<< "${LINKSET}"
else
    echo "stdlib link set: $(head -c 600 ls.log)" > cc.log
fi
if [ "${#LINK[@]}" -eq 0 ] || ! "${CC}" "${CF[@]}" -o t t.c "${LINK[@]}" -lm -lpthread >> cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing"; FAIL=$((FAIL + 1))
    fi
fi

# ── Part 2: end to end ───────────────────────────────────────────────────
cat > d.tk <<'TOKE'
m=d;i=io:std.io;i=json:std.json;
f=main():i64{
  let o=mt json.dec("{\"n\":-1,\"f\":1.5,\"e\":1e3,\"p\":7}") { $ok:v v; $jsonerr:e $jsonval{raw:""} };
  let a=mt json.u64(o;"n") { $ok:v "ok"; $jsonerr:e mt e { $type:m "type"; $missing:m "missing"; $parse:m "parse" } };
  let b=mt json.u64(o;"f") { $ok:v "ok"; $jsonerr:e mt e { $type:m "type"; $missing:m "missing"; $parse:m "parse" } };
  let c=mt json.i64(o;"f") { $ok:v "ok"; $jsonerr:e mt e { $type:m "type"; $missing:m "missing"; $parse:m "parse" } };
  let d=mt json.i64(o;"e") { $ok:v "ok"; $jsonerr:e mt e { $type:m "type"; $missing:m "missing"; $parse:m "parse" } };
  let g=mt json.i64(o;"n") { $ok:v v; $jsonerr:e 99 };
  let p=mt json.u64(o;"p") { $ok:v v; $jsonerr:e 0 as u64 };
  io.println("u64(-1)=\(a) u64(1.5)=\(b) i64(1.5)=\(c) i64(1e3)=\(d) i64(-1)=\(g) u64(7)=\(p)");
  <0
};
TOKE
want='u64(-1)=type u64(1.5)=type i64(1.5)=type i64(1e3)=type i64(-1)=-1 u64(7)=7'
if ! TKC_STDLIB_DIR="${S}" "${TKC}" --out d d.tk > d.log 2>&1; then
    echo "  FAIL: d.tk did not compile"; head -4 d.log | sed 's/^/        /'; FAIL=$((FAIL + 1))
else
    got="$(./d 2>&1)"
    for part in ${want}; do
        if printf '%s\n' "${got}" | tr ' ' '\n' | grep -qxF "${part}"; then
            echo "  PASS: e2e ${part}"; PASS=$((PASS + 1))
        else
            echo "  FAIL: e2e expected ${part}, got '${got}'"; FAIL=$((FAIL + 1))
        fi
    done
fi

echo "---------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
