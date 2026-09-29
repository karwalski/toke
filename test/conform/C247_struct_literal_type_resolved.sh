#!/usr/bin/env bash
# C247_struct_literal_type_resolved.sh — a struct literal's type name is resolved
# (story 138.9, loke T-42).
#
# The literal's own token is its type name and name resolution never looked at
# it: `$ghost{name:"a"}`, a type declared nowhere, was accepted, typed unknown,
# and read positionally through whatever struct it later met (`r.label`
# printed the ghost's first field).  It is now E3011 with the usual
# kind-aware did-you-mean.
#
# Negative control: TKC=<main's toke> -- cases 1 and 2 fail (rc=0).
#
# Story: 138.9 (T-42)

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
# code|got|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
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
expect "1 a literal of an undeclared struct type (loke T-42)" \
    "$(diags 'm=q;t=$real{label:str;value:str};t=$box{items:@$real};f=main():i64{let xs=@($ghost{name:"a";value:"b"});let b=$box{items:xs};<0};')" "E3011||ABSENT"
expect "2 a misspelt struct type gets the did-you-mean" \
    "$(diags 'm=q;t=$pt{x:i64};f=main():i64{let p=$pnt{x:1};<0};')" "E3011||did you mean 'pt'?"
expect "3 declared struct and sum literals stay clean" \
    "$(diags 'm=q;t=$pt{x:i64};t=$lvl{$a:i64;$b:i64};f=main():i64{let p=$pt{x:1};let l=$lvl{$a:0};<p.x};')" "rc=0"
expect "4 an imported type's literal stays clean" \
    "$(diags 'm=q;i=h:std.http;f=main():i64{let r=$res{status:200;body:"x";headers:@()};<0};')" "rc=0"

echo "C247: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
