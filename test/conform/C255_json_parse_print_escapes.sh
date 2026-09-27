#!/usr/bin/env bash
# C255_json_parse_print_escapes.sh — j.parse decodes a JSON string's escapes
# and j.print writes every control byte escaped (story 127.229 item 3).
#
# THE DEFECT.  tk_json_parse (src/stdlib/tk_runtime.c) copied the bytes
# between the quotes verbatim: `"a\"b"` parsed as the two characters `a\`
# (the escaped quote ENDED the string), and `\\`, `\n`, `\u00e9` were kept as
# their escape text.  The array parser's element count ignored strings, so
# `["a,b"]` had two elements.  On the way out, tk_json_print_str escaped only
# '"' and '\', and json_enc (every element of a printed @str) wrote control
# characters raw — a parsed "x\ny" printed with a real newline inside the
# string literal, which is not JSON.
#
# WHAT IS ASSERTED: a compiled program reads argv[1] with j.parse and prints
# the length and the value back with j.print, for a str and for an @str.
# Each output line must be exactly the expected JSON.
#
# Negative control: build against main's tk_runtime.c and json.c
# (C255_STDLIB=<dir with those files>) -> every escape case fails.
#
# Story: 127.229
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C255_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c255_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C255: j.parse decodes escapes, j.print escapes control bytes (127.229)"
echo "-----------------------------------------------------------------------"

cat > s.tk <<'TOKE'
m=main;i=j:std.json;i=s:std.str;f=main():i64{let t:str=j.parse(s.argv(1));j.print(t.len as i64);j.print(t);<0};
TOKE
cat > a.tk <<'TOKE'
m=main;i=j:std.json;i=s:std.str;f=main():i64{let t:@str=j.parse(s.argv(1));j.print(t.len as i64);j.print(t);<0};
TOKE
for p in s a; do
    if ! "${TKC}" --allow-all --out "${p}_bin" "${p}.tk" > "${p}.log" 2>&1; then
        echo "  FAIL: ${p}.tk did not compile"; sed 's/^/      /' "${p}.log" | head -5
        echo "C255: 0 passed, 1 failed"; exit 1
    fi
done

BS='\'   # one backslash (kept out of literals that tools rewrite)
# case BIN INPUT EXPECTED(two lines joined by '|') DESCRIPTION
case_() {
    local got
    got="$("./$1" "$2" 2>&1 | awk 'BEGIN{ORS=""} NR>1{print "|"} {print}')"
    if [ "${got}" = "$3" ]; then
        echo "  PASS: $4"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $4"; echo "        input:    $2"
        echo "        expected: $3"; echo "        got:      ${got}"; FAIL=$((FAIL + 1))
    fi
}

case_ s_bin '"plain"'               '5|"plain"'           'a plain string is unchanged'
case_ s_bin '"a\"b"'                '3|"a\"b"'            '\" is a quote inside the string, not its end'
case_ s_bin '"a\\b"'                '3|"a\\b"'            '\\ is one backslash, printed escaped'
case_ s_bin '"x\ny"'                '3|"x\ny"'            '\n decodes to a newline and prints as \n'
case_ s_bin '"t\u0001"'             '2|"t\u0001"'         '\u0001 decodes to one byte and prints escaped'
case_ s_bin "\"${BS}u00e9A\""         "3|\"$(printf '\303\251')A\"" "${BS}u00e9 decodes to 2 bytes of UTF-8"
case_ s_bin "\"${BS}ud83d${BS}ude00\""  "4|\"$(printf '\360\237\230\200')\"" 'a surrogate pair decodes to one code point'
case_ a_bin '["a,b","c\"]d","x\ny"]' '3|["a,b","c\"]d","x\ny"]' '@str: , ] and escapes inside elements are text; control bytes escaped'

echo "-----------------------------------------------------------------------"
echo "C255: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
