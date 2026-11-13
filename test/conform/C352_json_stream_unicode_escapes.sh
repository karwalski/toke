#!/usr/bin/env bash
# C352_json_stream_unicode_escapes.sh — the json stream parser decodes
# \uXXXX (story 135.27).
#
# THE DEFECT.  json.c's stream_parse_string (json_streamnext's Key and Str
# tokens) had its own escape table with no 'u' case: "\u00e9" came out as
# the five bytes "u00e9", and a surrogate pair as two such runs.  Same class
# as T-47a, which 138.12 fixed for json.str/tk_json_parse by routing them
# through strbuf_append_json_unescape.  The stream parser now uses that one
# decoder too.
#
# The stream API is not reachable from a toke program (quarantined, no _w
# glue: scripts/check_tki_skiplist.txt, 102.16), so this drives the C entry
# points json_streamparser / json_streamnext directly.
#
# WHAT IS ASSERTED (bytes printed as hex, one token per line):
#   1. "\u00e9"           -> c3a9            (BMP code point as UTF-8)
#   2. "\ud83d\ude00"     -> f09f9880        (surrogate pair combined)
#   3. key "k\u00e9y"     -> 6bc3a979        (Key tokens decode too)
#   4. "q\"x\\y\/\n"      -> 7122785c792f0a  (the short escapes, unchanged)
#   5. "a\u0000b"         -> 61efbfbd62      (NUL becomes U+FFFD, as in the
#                                             shared decoder)
#   6. a string after all of that still tokenises: "end" -> 656e64
#
# Negative control (run 2026-09-28): C352_ROOT=<a main 8f63fc6 worktree>
# builds the harness from main's json.c: 1, 2, 3 and 5 fail ("u00e9" etc.
# verbatim).
#
# Story: 135.27
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="${C352_ROOT:-$(cd "${SCRIPT_DIR}/../.." && pwd)}"
CC="${CC:-cc}"
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c352_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
echo "C352: json stream parser decodes \\uXXXX (135.27)"
echo "------------------------------------------------------------------------"
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
cat > "${WORK}/st.c" <<'CEOF'
#include <stdio.h>
#include <string.h>
#include "json.h"
int main(void) {
    const char *in =
        "[\"\\u00e9\", \"\\ud83d\\ude00\", {\"k\\u00e9y\": 1},"
        " \"q\\\"x\\\\y\\/\\n\", \"a\\u0000b\", \"end\"]";
    JsonStream st = json_streamparser((const uint8_t *)in, (uint64_t)strlen(in));
    for (int i = 0; i < 64; i++) {
        JsonTokenResult tr = json_streamnext(&st);
        if (tr.is_err) { printf("ERR %s\n", tr.err.msg ? tr.err.msg : ""); return 1; }
        if (tr.ok.kind == JSON_TOK_END) return 0;
        if (tr.ok.kind == JSON_TOK_STR || tr.ok.kind == JSON_TOK_KEY) {
            for (const unsigned char *p = (const unsigned char *)tr.ok.val.str; *p; p++)
                printf("%02x", *p);
            printf("\n");
        }
    }
    return 1;
}
CEOF
# 127.295: json.c reads lengths with tk_strlen, whose string-heap state lives
# in str.c, so std.json's link set comes from stdlib_link_set.sh --objects,
# never a hand list.
CF=(-std=c99 -D_GNU_SOURCE -iquote "${REPO_ROOT}/src/stdlib" -I"${REPO_ROOT}/src/stdlib")
LINK=()
if LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${REPO_ROOT}/src/stdlib" \
        --cc "${CC}" --objects json -- "${CF[@]}" 2>"${WORK}/ls.log")"; then
    while IFS= read -r l; do [ -n "$l" ] && LINK+=("$l"); done <<< "${LINKSET}"
else
    echo "stdlib link set: $(head -c 400 "${WORK}/ls.log")" > "${WORK}/cc.log"
fi
if [ "${#LINK[@]}" -eq 0 ] || ! "${CC}" "${CF[@]}" -o "${WORK}/st" "${WORK}/st.c" \
        "${LINK[@]}" -lm -lpthread >>"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: harness does not build: $(head -c 400 "${WORK}/cc.log")"
    echo ""; echo "C352: 0 passed, 1 failed"; exit 1
fi
"${WORK}/st" > "${WORK}/out.txt"; rc=$?
expect "stream parses to End" "${rc}" "0"
line() { sed -n "$1p" "${WORK}/out.txt"; }
expect "1: \\u00e9 -> UTF-8"               "$(line 1)" "c3a9"
expect "2: surrogate pair -> one code point" "$(line 2)" "f09f9880"
expect "3: Key token decodes \\u"          "$(line 3)" "6bc3a979"
expect "4: short escapes unchanged"        "$(line 4)" "7122785c792f0a"
expect "5: \\u0000 -> U+FFFD"              "$(line 5)" "61efbfbd62"
expect "6: next string intact"             "$(line 6)" "656e64"
echo ""
echo "C352: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
