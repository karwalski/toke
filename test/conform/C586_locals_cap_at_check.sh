#!/usr/bin/env bash
# C586_locals_cap_at_check.sh — the max_locals cap is met at --check, by a
# lower-bound count shared through src/tkc_caps.h (story 127.252 residue).
#
# THE GAP.  127.252 (1) made codegen report the max_locals caps once per
# function, located (C560) -- but only while emitting: `--check` passed a
# function with 129 lets, and the build then failed E9010.  tkc_caps.h already
# met max_funcs / max_structs at --check (138.15, C480); it now also counts,
# per function, what certainly holds a local slot until the function ends:
# parameters, `let`/`let x=mut.` bindings and `lp(let i=..)`.  Match-arm
# subtrees and closure bodies are left out (codegen can release or lift
# them), so the count is a LOWER bound of codegen's: --check refuses only
# what the build refuses too, in codegen's words ("needs at least").
#
# WHAT THIS DRIVES.  (1) exactly max_locals (128) lets: --check and build
# clean; (2) 129: one E9010 at --check, located at the f=, expected 128 /
# got 129, no fix; (3) parameters count; (4) sibling blocks do not share
# slots (140 lets -> 141 with the flag); (5) --max-locals=129 admits (2);
# (6) the lower bound: 127 lets + a match whose arm bindings codegen still
# counts passes --check and is refused by the build -- the checker never
# claims a slot it cannot prove.
#
# Negative control: delete `+ tkc_caps_locals_check(...)` from
# tkc_caps_check in src/tkc_caps.h — cases 2-4 fail (--check exits 0).
#
# Story: 127.252 (residue)

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c586_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C586: max_locals met at --check (lower bound)"
echo "---------------------------------------------"

lets() {  # lets FROM TO PREFIX
    python3 -c "import sys; a,b=int(sys.argv[1]),int(sys.argv[2]); print(''.join('let %s%d=%d;'%(sys.argv[3],i,i) for i in range(a,b)))" "$@"
}

# check_one NAME SRC FLAGS WANT — WANT is "clean" or "E9010:<need>:<line>"
check_one() {
    printf '%s\n' "$2" > "${WORK}/$1.tk"
    local rc=0
    # shellcheck disable=SC2086
    "${TKC}" --check $3 "${WORK}/$1.tk" >"${WORK}/$1.o" 2>&1 || rc=$?
    if [ "$4" = clean ]; then
        if [ "${rc}" = 0 ]; then pass "$1: --check clean"
        else fail "$1: --check rc ${rc}: $(head -c 200 "${WORK}/$1.o")"; fi
        return
    fi
    local need line; need="$(echo "$4" | cut -d: -f2)"; line="$(echo "$4" | cut -d: -f3)"
    local n; n="$(grep -c '"error_code":' "${WORK}/$1.o")"
    if [ "${rc}" = 1 ] && [ "${n}" = 1 ] &&
       grep -q "needs at least ${need} local-variable slots and the limit is 128 (max_locals)" "${WORK}/$1.o" &&
       grep -q "\"line\":${line}," "${WORK}/$1.o" &&
       grep -q '"expected":"128"' "${WORK}/$1.o" && grep -q "\"got\":\"${need}\"" "${WORK}/$1.o" &&
       ! grep -q '"fix"' "${WORK}/$1.o"; then
        pass "$1: one E9010 at --check, line ${line}, needs at least ${need}, no fix"
    else
        fail "$1: rc ${rc}, ${n} records: $(head -c 300 "${WORK}/$1.o")"
    fi
}

build_rc() {  # build_rc NAME -> prints the build's exit code
    local rc=0
    "${TKC}" --out "${WORK}/$1" "${WORK}/$1.tk" >"${WORK}/$1.b" 2>&1 || rc=$?
    echo "${rc}"
}

check_one at_cap "m=c586;
f=main():i64{$(lets 0 128 v)<v0};" "" clean
r="$(build_rc at_cap)"
if [ "${r}" = 0 ]; then pass "at_cap: builds"; else fail "at_cap: build rc ${r}"; fi

check_one over_cap "m=c586;
f=main():i64{$(lets 0 129 v)<v0};" "" "E9010:129:2"

check_one params "m=c586;
f=g(a:i64;b:i64):i64{$(lets 0 127 v)<a};
f=main():i64{<g(1;2)};" "" "E9010:129:2"

check_one blocks "m=c586;
f=main():i64{let c=true; if(c){$(lets 0 70 a)}; if(c){$(lets 0 70 a)}; <0};" "" "E9010:141:2"

check_one raised "m=c586;
f=main():i64{$(lets 0 129 v)<v0};" "--max-locals=129" clean

check_one arm_lower_bound "m=c586;
t=\$ue{\$bad:str};
f=g(n:i64):i64!\$ue{<n};
f=main():i64{$(lets 0 127 v) let r=mt g(1) {\$ok:x x; \$err:e 0}; <r};" "" clean
r="$(build_rc arm_lower_bound)"
if [ "${r}" != 0 ] && grep -q "needs at least" "${WORK}/arm_lower_bound.b"; then
    pass "arm_lower_bound: codegen still refuses it (the check is a lower bound)"
else
    fail "arm_lower_bound: build rc ${r}: $(head -c 200 "${WORK}/arm_lower_bound.b")"
fi

echo ""
echo "C586: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
