#!/usr/bin/env bash
# C218_arg_mismatch_fix_certain.sh — an argument E4031 names the array's
# element type and offers `as` only where `as` is the repair
# (story 127.220 item 4).
#
# A `[byte]` passed to a `str` parameter printed "got '@(?)'" — the checker's
# resolve_type knew `Byte` but not the lower-case `byte` the spec and the
# .tki files spell — and carried fix "cast argument to str using 'as'",
# which `as` cannot do for an array.  AGENTS §6: a fix is present only when
# it is correct in every case.  Now the fix is present only between numeric
# types, never float -> integer (the 127.64 rule: truncation changes the
# value).
#
# Negative control: TKC=<main's toke> -- cases 1, 2 and 4 fail.
#
# Story: 127.220 (item 4)

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
# code|got|fix-or-ABSENT of each diagnostic, one per line joined by ' / '
diags() { "${TKC}" --check "$1" 2>&1 | python3 -c '
import sys, json
out = []
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out))'; }
prog() {  # $1 = binding, $2 = call
    printf 'm=q;\ni=s:std.str;\nf=g(x:str):i64{<s.len(x)};\nf=h(x:f64):i64{<0};\nf=k(x:i64):i64{<0};\nf=main():i64{\n  %s\n  <%s\n};\n' "$1" "$2" > "${WORK}/p.tk"
    diags "${WORK}/p.tk"; }

expect "1 [byte] as str: element type named, no fix" \
    "$(prog 'let b:@(byte)=s.bytes("hi");' 'g(b)')" "E4031|@(u8)|ABSENT"
expect "2 i64 as str: no fix (as is not a certain repair)" \
    "$(prog 'let b:i64=3;' 'g(b)')" "E4031|i64|ABSENT"
expect "3 i64 as f64: the cast is the repair" \
    "$(prog 'let b:i64=3;' 'h(b)')" "E4031|i64|cast argument to f64 using 'as'"
expect "4 f64 as i64: no fix (truncation)" \
    "$(prog 'let c:f64=3.5;' 'k(c)')" "E4031|f64|ABSENT"

echo "C218: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
