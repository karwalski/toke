#!/usr/bin/env bash
# C694 -- a diagnostic's source_line survives a raw NUL or control byte
# (127.267).
#
# diag.c copied the source line into a buffer and then treated it as a C
# string, so a raw NUL ended it.  That is reachable since 131.32: W1021 warns
# on exactly such a byte inside a string literal, and the record W1021 emits
# carried the line cut off just before the byte it was reporting:
#     "source_line":"  let x=\"a"
# The line is now written by length.  In JSON every byte below U+0020, and
# DEL, is \u00XX; the text renderer prints the whole line with each control
# byte shown as '?' (one column, so the caret still lines up).
#
# Every record is parsed with a real JSON parser, so an unescaped byte that
# made the record invalid would fail the case, not pass it.
#
# Negative control: TKC=<main 13f1d3ae's toke> -- cases 1-4 fail (the line
# ends at the NUL); case 5 (an ordinary line) passes on both.
#
# Story: 127.267

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# "CODE <python repr of source_line>" for every JSON record tkc --check prints
lines() {
    "${TKC}" --check "$1" 2>&1 >/dev/null | python3 -c '
import json, sys
for l in sys.stdin:
    l = l.strip()
    if not l: continue
    try: r = json.loads(l)
    except ValueError: print("INVALID-JSON"); continue
    print(r["error_code"], repr(r.get("source_line")))'
}

printf 'm=h;\ni=io:std.io;\nf=main():i64{\n  let x="a\000b";\n  io.println(x);\n  <0\n};\n' > "${WORK}/nul.tk"
expect "1 W1021 on a raw NUL: source_line is the whole line, the NUL as \\u0000" \
    "$(lines "${WORK}/nul.tk")" \
    "W1021 '  let x=\"a\\x00b\";'"

printf 'm=h;\ni=io:std.io;\nf=main():i64{\n  let x="a\000b\001c\177d\037e";\n  io.println(x);\n  <0\n};\n' > "${WORK}/mix.tk"
expect "2 NUL, 0x01, DEL and 0x1F on one line: every record carries all of it" \
    "$(lines "${WORK}/mix.tk" | sort -u)" \
    "W1021 '  let x=\"a\\x00b\\x01c\\x7fd\\x1fe\";'"

printf 'm=h;\nf=main():i64{\n  let x=1;\000 <x\n};\n' > "${WORK}/bare.tk"
expect "3 a raw NUL outside a literal (E1003): the line runs past it" \
    "$(lines "${WORK}/bare.tk")" \
    "E1003 '  let x=1;\\x00 <x'"

"${TKC}" --check --diag-text "${WORK}/nul.tk" > "${WORK}/text" 2>&1
expect "4 --diag-text prints the whole line, the byte as '?', caret under it" \
    "$(sed -n '4,5p' "${WORK}/text")" \
    "$(printf '4 |   let x="a?b";\n  |           ^ replace the raw byte with \\x00')"

printf 'm=h;\nf=main():i64{\n  let s="q\\"\\\\\tz";\n  <undefinedname\n};\n' > "${WORK}/plain.tk"
expect "5 an ordinary line is unchanged (quote, backslash escapes)" \
    "$(lines "${WORK}/plain.tk" | head -1)" \
    "E3011 '  <undefinedname'"

echo "C694: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
