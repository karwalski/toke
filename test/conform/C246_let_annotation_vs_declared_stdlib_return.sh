#!/usr/bin/env bash
# C246_let_annotation_vs_declared_stdlib_return.sh — an annotated `let` is
# checked against the scalar return an imported interface declares (story
# 127.232 item 1).
#
# stdlib returns are not typed by the checker (127.66), so
# `let x:bool=math.floor(3.9)` passed --check while `let x:bool=1.5` was
# E4031 -- although the emitter lowers that same declaration (math.floor ->
# double), and `let y:str=math.floor(2.5)` failed the build with E9003.
# Only a hand-written record's scalar return is used; a `"gen":"abi"` record's
# `i64` is a placeholder (127.220 item 5) and is never judged.
#
# Negative control: TKC=<main's toke> -- cases 1-2 fail (rc=0).
#
# Story: 127.232 (item 1)

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
    out.append("%s|%s|%s|%s" % (d.get("error_code"), d.get("expected"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
H='m=q;i=math:std.math;i=s:std.str;'

expect "1 bool annotation over a declared f64 return" \
    "$(diags "${H}"'f=main():i64{let x:bool=math.floor(3.9);<0};')" "E4031|bool|f64|ABSENT"
expect "2 str annotation over a declared f64 return" \
    "$(diags "${H}"'f=main():i64{let y:str=math.floor(2.5);<0};')" "E4031|str|f64|ABSENT"
expect "3 matching annotations stay clean (f64, bool, u64 into i64)" \
    "$(diags "${H}"'f=main():i64{let z:f64=math.floor(1.5);let b:bool=s.contains("ab";"a");let n:i64=s.len("ab");<0};')" "rc=0"
expect "4 a gen:abi placeholder return is not judged" \
    "$(diags "${H}"'f=main():i64{let t:str=s.nowunix();<0};')" "rc=0"

echo "C246: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
