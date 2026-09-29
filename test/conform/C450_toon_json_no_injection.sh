#!/usr/bin/env bash
# C450_toon_json_no_injection.sh — std.toon's JSON converters escape what
# they copy across, in both directions (story 121.40).
#
# THE DEFECT.  toon_to_json wrote each field name and each string value
# between double quotes with no JSON escaping, so the TOON value
#     x","admin":true,"y
# came out as three JSON members, one of them "admin":true.  In the other
# direction toon_from_json copied JSON string bodies into TOON raw: a '|' in
# a value added a column, an escaped newline stayed as the two bytes "\n"
# (and a real one would have started a new row), and every '{' anywhere in
# the input -- including inside a string -- was counted as, and walked as, a
# new object.  Measured on main 8efba9ea before 121.40:
#   to_json value  x","admin":true,"y   -> [{"name":"x","admin":true,"y"}]
#   to_json key    a","admin":true,"b   -> [{"a"":"v",""admin":true":"",...
#   to_json 1.2.3                      -> [{"v":1.2.3}]   (not JSON)
#   from_json {"a":"x|y","b":"z"}      -> a=x, b=y        (z lost)
#   from_json {"a":"p{q"}              -> data[2], two rows
#
# THE RULE NOW.  TOON text uses one escape character, '\': inside a value
# \\ \| \n \r stand for the byte they name; inside a field name \, \{ \}
# too.  toon_from_json decodes each JSON string (strbuf_append_json_unescape)
# and TOON-escapes it; toon_to_json splits rows and schemas escape-aware,
# decodes, and JSON-escapes names and string values (strbuf_append_json).
# A value is emitted bare only when it is a JSON number, true, false or null.
# toon.str decodes too, so a value survives JSON -> TOON -> toon.str.
#
# Every case is built with -fsanitize=address,undefined and
# -fno-sanitize-recover, so a memory error is a hard failure.
#
# Negative control: C450_STDLIB=<dir> points the suite at another stdlib
# tree.  A copy of src/stdlib/ with main's toon.c put back fails the
# injection cases.
#
# Story: 121.40

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C450_STDLIB:-${REPO_ROOT}/src/stdlib}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c450_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C450: std.toon JSON converters escape both directions"
echo "-----------------------------------------------------"

cat > "${WORK}/t.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "toon.h"

static int bad = 0;
static void eq(const char *what, const char *got, const char *want) {
    if (got && !strcmp(got, want)) { printf("OK %s\n", what); return; }
    printf("BAD %s\n      got:  %s\n      want: %s\n", what, got ? got : "(null)", want);
    bad++;
}
/* JSON -> TOON -> JSON must give back the canonical JSON it started from. */
static void rt(const char *what, const char *json, const char *want) {
    const char *t = toon_from_json(json);
    eq(what, toon_to_json(t), want);
}

int main(void) {
    /* ---- toon_to_json: nothing in a TOON document becomes JSON syntax ---- */
    eq("to_json: a value cannot add a member",
       toon_to_json("{name}:\nx\",\"admin\":true,\"y\n"),
       "[{\"name\":\"x\\\",\\\"admin\\\":true,\\\"y\"}]");
    /* the raw commas split it into three TOON fields; each stays a key */
    eq("to_json: a field name cannot add a member",
       toon_to_json("{a\",\"admin\":true,\"b}:\nv\n"),
       "[{\"a\\\"\":\"v\",\"\\\"admin\\\":true\":\"\",\"\\\"b\":\"\"}]");
    eq("to_json: backslash, tab and a control byte are escaped",
       toon_to_json("{p}:\nC:\\dir\tx\x01y\n"),
       "[{\"p\":\"C:\\\\dir\\tx\\u0001y\"}]");
    eq("to_json: only JSON numbers are bare",
       toon_to_json("{a,b,c,d,e,f}:\n1.2.3|-|.|-0.5|12|1e3\n"),
       "[{\"a\":\"1.2.3\",\"b\":\"-\",\"c\":\".\",\"d\":-0.5,\"e\":12,\"f\":1e3}]");
    eq("to_json: TOON escapes decode (\\| \\n \\\\)",
       toon_to_json("{a,b}:\nx\\|y\\nz\\\\|w\n"),
       "[{\"a\":\"x|y\\nz\\\\\",\"b\":\"w\"}]");
    eq("to_json: escaped comma and brace in a field name",
       toon_to_json("{a\\,b,c\\}d}:\n1|2\n"),
       "[{\"a,b\":1,\"c}d\":2}]");

    /* ---- toon_from_json: nothing in a JSON string becomes TOON syntax ---- */
    eq("from_json: '|' in a value is escaped, not a column",
       toon_from_json("[{\"a\":\"x|y\",\"b\":\"z\"}]"),
       "data[1]{a,b}:\nx\\|y|z\n");
    eq("from_json: a newline in a value is escaped, not a row",
       toon_from_json("[{\"a\":\"l1\\nl2\"}]"),
       "data[1]{a}:\nl1\\nl2\n");
    eq("from_json: '{' inside a string is not an object",
       toon_from_json("[{\"a\":\"p{q\"},{\"a\":\"r\"}]"),
       "data[2]{a}:\np{q\nr\n");
    eq("from_json: a quote in a key does not end the key",
       toon_from_json("[{\"k\\\"x\":1}]"),
       "data[1]{k\"x}:\n1\n");
    eq("from_json: ',' and '}' in a key are escaped",
       toon_from_json("{\"a,b\":1,\"c}\":2}"),
       "{a\\,b,c\\}}:\n1|2\n");
    eq("from_json: single object, '|' in a value",
       toon_from_json("{\"a\":\"x|y\",\"b\":\"z\"}"),
       "{a,b}:\nx\\|y|z\n");
    eq("from_json: JSON escapes are decoded (\\u00e9, \\\")",
       toon_from_json("[{\"a\":\"caf\\u00e9 \\\"q\\\"\"}]"),
       "data[1]{a}:\ncaf\xc3\xa9 \"q\"\n");

    /* ---- the two directions round-trip ---- */
    rt("round trip: the injection payload stays one string",
       "[{\"name\":\"x\\\",\\\"admin\\\":true,\\\"y\"}]",
       "[{\"name\":\"x\\\",\\\"admin\\\":true,\\\"y\"}]");
    rt("round trip: pipes, newlines, backslashes, braces",
       "[{\"a\":\"1|2\\n3\\\\4{5}\",\"b\":7},{\"a\":\"\",\"b\":8}]",
       "[{\"a\":\"1|2\\n3\\\\4{5}\",\"b\":7},{\"a\":\"\",\"b\":8}]");
    rt("round trip: awkward keys",
       "[{\"a,b\":1,\"c}d\":2,\"e|f\":3}]",
       "[{\"a,b\":1,\"c}d\":2,\"e|f\":3}]");

    /* ---- accessors read what from_json wrote ---- */
    ToonResult d = toon_dec(toon_from_json("[{\"a\":\"x|y\\nz\",\"b\":\"w\",\"k,1\":5}]"));
    StrToonResult s = toon_str(d.ok, "a");
    eq("toon.str decodes an escaped value", s.is_err ? NULL : s.ok, "x|y\nz");
    s = toon_str(d.ok, "b");
    eq("toon.str: the next column is still the next column", s.is_err ? NULL : s.ok, "w");
    I64ToonResult n = toon_i64(d.ok, "k,1");
    printf("%s toon.i64 finds an escaped field name\n", (!n.is_err && n.ok == 5) ? "OK" : "BAD");
    if (n.is_err || n.ok != 5) bad++;

    /* ---- toon.arr: the header count cannot outrun the rows ----
     * Before the fix, data[5] over two rows returned len 5 with three
     * uninitialised elements, and a blank line took a row's slot. */
    ToonResult ad = toon_dec("data[5]{a}:\n1\n\n2\n");
    ToonArrayResult ar = toon_arr(ad.ok, "a");
    int arr_ok = !ar.is_err && ar.ok.len == 2 &&
                 !strcmp(ar.ok.data[0].raw, "1") && !strcmp(ar.ok.data[1].raw, "2");
    printf("%s toon.arr: data[5] over two rows (one blank line) gives two elements\n", arr_ok ? "OK" : "BAD");
    if (!arr_ok) bad++;

    /* ---- plain data is unchanged ---- */
    eq("plain: from_json array", toon_from_json("[{\"id\":1,\"name\":\"Alice\"},{\"id\":2,\"name\":\"Bob\"}]"),
       "data[2]{id,name}:\n1|Alice\n2|Bob\n");
    eq("plain: to_json", toon_to_json("data[2]{id,name,ok}:\n1|Alice|true\n2|Bob|null\n"),
       "[{\"id\":1,\"name\":\"Alice\",\"ok\":true},{\"id\":2,\"name\":\"Bob\",\"ok\":null}]");
    return bad ? 1 : 0;
}
C

if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" \
        -o "${WORK}/t" "${WORK}/t.c" "${S}/toon.c" >"${WORK}/t.cc" 2>&1; then
    fail "build: $(grep -a -m3 -E 'error' "${WORK}/t.cc" | head -c 400)"
else
    "${WORK}/t" >"${WORK}/t.out" 2>&1
    rc=$?
    while IFS= read -r line; do
        case "${line}" in
            "OK "*)  pass "${line#OK }" ;;
            "BAD "*) fail "${line#BAD }" ;;
            *)       echo "${line}" ;;
        esac
    done < "${WORK}/t.out"
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^BAD ' "${WORK}/t.out"; then
        fail "exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/t.out" | head -c 300)"
    fi
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ] && [ "${PASS}" -gt 0 ]
