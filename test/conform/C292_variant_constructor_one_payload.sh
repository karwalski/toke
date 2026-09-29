#!/usr/bin/env bash
# C292_variant_constructor_one_payload.sh — `$name(..)` with no function
# `name` is a variant constructor and carries exactly one payload (story
# 138.10, loke N-3).
#
# `<$ok()`, `$none()` and `$ok(1;2)` passed --check and reached clang as
# E9003 on an undefined `@ok`.  They are E4026 now.  `$foo()` naming a
# declared function is a call and stays clean (n3c, n3f).
#
# Negative control: TKC=<main's toke> -- cases 1-3 fail (rc=0).
#
# Story: 138.10 (N-3)

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

expect "1 <\$ok() in a void!\$str function" \
    "$(diags 'm=q;f=g():void!$str{<$ok()};f=main():i64{let r=g();<0};')" "E4026|0|ABSENT"
expect "2 \$none() on a sum whose \$none carries a payload" \
    "$(diags 'm=q;t=$sh{$circle:i64;$none:i64};f=g():$sh{<$none()};f=main():i64{let s=g();<0};')" "E4026|0|ABSENT"
expect "3 \$ok(1;2)" \
    "$(diags 'm=q;f=g():i64!$str{<$ok(1;2)};f=main():i64{<0};')" "E4026|2|ABSENT"
expect "4 \$ok(5) stays clean" \
    "$(diags 'm=q;f=g():i64!$str{<$ok(5)};f=main():i64{<0};')" "rc=0"
expect "5 \$foo() naming a declared function stays clean" \
    "$(diags 'm=q;f=foo():i64{<7};f=main():i64{<$foo()};')" "rc=0"
expect "6 \$ok() naming a declared function ok stays clean" \
    "$(diags 'm=q;f=ok():i64{<3};f=main():i64{<$ok()};')" "rc=0"

echo "C292: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
