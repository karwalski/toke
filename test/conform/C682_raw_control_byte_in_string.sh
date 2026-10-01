#!/usr/bin/env bash
# C682 -- a raw control byte in a string literal is W1021, with an exact fix
# (131.32).
#
# Four toke-test-programs solutions held raw 0x00 bytes inside string
# literals.  tkc accepted them silently; the corpus control-byte guard
# (toke-corpus regen/run_shard.py) rejects them.  The lexer now warns, once
# per byte, for the set that guard rejects -- 0x00-0x08, 0x0B, 0x0C,
# 0x0E-0x1A, 0x1C-0x1F -- and not for tab, LF, CR or ESC.  It is a warning
# because the spec permits any UTF-8 in string content (v0.4 §G).
#
# The fix is "replace the raw byte with \xHH".  It is exact: case 6 builds
# the program with the byte and with the escape and requires identical IR.
# This is the D-series check of the fix field (YAML input cannot hold the
# bytes).
#
# Negative control: TKC=<437dc471's toke> -- cases 1-4 fail (no W1021).
#
# Story: 131.32

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
prog() { printf 'm=h;\ni=io:std.io;\nf=main():i64{\n  let x="a%sb";\n  io.println(x);\n  <0\n};\n' "$1"; }

# "rc=N codes fixes" for --check of a program whose literal holds $1
chk() {
    prog "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    codes=$(grep -o '"error_code":"[A-Z0-9]*"' "${WORK}/o" | sed 's/.*:"\(.*\)"/\1/' | tr '\n' ' ')
    fixes=$(grep -o '"fix":"[^"]*"' "${WORK}/o" | tr '\n' ' ')
    echo "rc=${rc} ${codes}${fixes}"
}

printf 'm=h;\ni=io:std.io;\nf=main():i64{\n  let x="a\000b";\n  io.println(x);\n  <0\n};\n' > "${WORK}/n.tk"
"${TKC}" --check "${WORK}/n.tk" > "${WORK}/o" 2>&1; rc=$?
expect "1 a raw NUL is W1021, rc 0, and the fix names \\x00" \
    "rc=${rc} $(grep -o '"error_code":"[A-Z0-9]*"' "${WORK}/o") $(grep -o '"fix":"[^"]*"' "${WORK}/o")" \
    'rc=0 "error_code":"W1021" "fix":"replace the raw byte with \\x00"'
expect "2 0x01 and 0x1F: one warning per byte, each with its own fix" \
    "$(chk $'\x01\x1f')" \
    'rc=0 W1021 W1021 "fix":"replace the raw byte with \\x01" "fix":"replace the raw byte with \\x1F" '
expect "3 the position is the byte's" \
    "$(prog $'\x0b' > "${WORK}/p.tk"; "${TKC}" --check "${WORK}/p.tk" 2>&1 | grep -o '"pos":{[^}]*},"span_start":[0-9]*,"span_end":[0-9]*')" \
    '"pos":{"offset":42,"line":4,"col":11},"span_start":42,"span_end":43'
expect "4 the message names the byte" \
    "$(prog $'\x0c' > "${WORK}/p.tk"; "${TKC}" --check "${WORK}/p.tk" 2>&1 | grep -o '"message":"[^"]*"')" \
    '"message":"raw control byte 0x0C in string literal"'
expect "5 tab, CR and ESC are not reported" "$(chk $'\t\r\x1b')" "rc=0 "

# 6: the fix is exact -- the byte and its escape are the same string
ok=1
for b in 00 01 07 0b 1a 1c 1f; do
    printf 'm=h;\ni=io:std.io;\nf=main():i64{\n  let x="a\\x%sb";\n  io.println(x);\n  <0\n};\n' "$b" > "${WORK}/e.tk"
    printf 'm=h;\ni=io:std.io;\nf=main():i64{\n  let x="a'"\\x$b"'b";\n  io.println(x);\n  <0\n};\n' > "${WORK}/r.tk"
    "${TKC}" --emit-llvm "${WORK}/e.tk" -o "${WORK}/e.ll" >/dev/null 2>&1 || ok=0
    "${TKC}" --emit-llvm "${WORK}/r.tk" -o "${WORK}/r.ll" >/dev/null 2>&1 || ok=0
    cmp -s <(grep -v -e '^source_filename' -e '^; ModuleID' "${WORK}/e.ll") \
           <(grep -v -e '^source_filename' -e '^; ModuleID' "${WORK}/r.ll") || { ok=0; echo "  differs for 0x$b"; }
done
expect "6 applying the fix (\\xHH) leaves the IR identical, for 7 bytes" "${ok}" "1"

echo "C682: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
