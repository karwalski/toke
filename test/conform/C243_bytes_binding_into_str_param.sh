#!/usr/bin/env bash
# C243_bytes_binding_into_str_param.sh — a `[byte]` an interface declares
# (`str.bytes`) reaching a str parameter is E4031 whether or not it was bound
# with an annotation (story 127.230 item 5).
#
# stdlib returns are not typed by the checker (127.66), so an un-annotated
# `let b=s.bytes("x"); g(b)` -- and `g(s.bytes("x"))` itself -- passed a byte
# array into a str parameter with no diagnostic, while `let b:@(byte)=..`
# was E4031 (C218).  The type is read for this check only and never stored,
# so codegen of accepted programs cannot move.
#
# Negative control: TKC=<main's toke> -- cases 1-3 fail (rc=0).
#
# Story: 127.230 (item 5)

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
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
H='m=q;i=s:std.str;f=g(x:str):i64{<s.len(x)};'

expect "1 un-annotated bytes binding into a str parameter" \
    "$(diags "${H}"'f=main():i64{let b=s.bytes("abc");<g(b)};')" "E4031|@(u8)|ABSENT"
expect "2 bytes call directly into a str parameter" \
    "$(diags "${H}"'f=main():i64{<g(s.bytes("abc"))};')" "E4031|@(u8)|ABSENT"
expect "3 through a copy of the binding" \
    "$(diags "${H}"'f=main():i64{let b=s.bytes("abc");let c=b;<g(c)};')" "E4031|@(u8)|ABSENT"
expect "4 a str-returning call stays clean" \
    "$(diags "${H}"'f=main():i64{let t=s.trim(" a ");<g(t)};')" "rc=0"
expect "5 bytes into a [byte] consumer stays clean" \
    "$(diags "${H}"'f=main():i64{let b=s.bytes("abc");let t=s.frombytes(b);<g(t)};')" "rc=0"

echo "C243: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
