#!/usr/bin/env bash
# C2250_arena_escape_check.sh — the {arena} escape check reaches the block's
# statements (story 127.526), and an {arena} block takes one optional trailing
# ';' (story 127.527).
#
# THE DEFECTS.
#   127.526: the E5001 check in src/types.c (NODE_ARENA_STMT) looked for
#     assignments among the block's children, but the parser gives the block
#     ONE child, its NODE_STMT_LIST, so no assignment was ever seen and no
#     input drew E5001: `m=test;let g=mut."";f=k():i64{{arena g="x"}<0};`
#     checked clean and built.
#   127.527: the parser returned after the block's '}' without the optional
#     ';' that `if` and `lp` take, so `{arena let x=1;x};<1` was E2002.
#
# THE RULE (docs/spec/memory-model.md §2.4).  Inside an {arena} block, at any
#   depth, a value of a heap type (str, array, map, struct or sum, closure, or
#   an error union whose value is one) assigned to a module-level `mut` is
#   E5001, with no `fix`.  A scalar is never allocated, so assigning one is
#   not an escape; a store to a binding of the enclosing function is not
#   checked (the {arena} ADR's open question, ADR-0016 Q2).
#
# WHAT IS ASSERTED.
#   Part 1 (build, not only --check): each escaping shape is refused with
#     exactly one diagnostic, E5001, stage arena_check, no `fix`, and no
#     binary is written.
#   Part 2: the accepted shapes build and print the expected output: a
#     scalar to a module `mut`, a str to an outer local, a shadowing local,
#     and `{arena ...};` / `{arena ...}` with and without the ';'.
#
# Negative control (measured 2026-11-24 on fix/127.526-arena-check against
#   its base 3a8f0ae6): Part 1 fails (every escape builds, no E5001) and the
#   ';' cases of Part 2 fail with E2002.
#
# Story: 127.526, 127.527
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2250_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C2250: {arena} escape check (127.526) and optional ';' (127.527)"
echo "---------------------------------------------------------------------------"
ok()  { echo "  PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

# refused NAME SOURCE GOT — the build fails with exactly one E5001 whose `got`
# is GOT, and no `fix`.
refused() {
    local name="$1" src="$2" got="$3"
    printf '%s\n' "${src}" > "${name}.tk"
    rm -f "${name}"
    if "${TKC}" --diag-json "${name}.tk" --out "${name}" > "${name}.out" 2>&1; then
        bad "${name}: built (expected E5001)"; return; fi
    local n e5 fx
    n=$(grep -c '"error_code"' "${name}.out")
    e5=$(grep -c '"error_code":"E5001","severity":"error","stage":"arena_check"' "${name}.out")
    fx=$(grep -c '"fix"' "${name}.out")
    if [ "${n}" = 1 ] && [ "${e5}" = 1 ] && [ "${fx}" = 0 ] \
        && grep -q "\"got\":\"${got}\"" "${name}.out" && [ ! -e "${name}" ]; then
        ok "${name}: E5001 (got ${got}), no fix"
    else
        bad "${name}: diagnostics=${n} e5001=${e5} fix=${fx}: $(head -c 300 "${name}.out")"
    fi
}

# accepted NAME SOURCE OUTPUT — builds and prints OUTPUT.
accepted() {
    local name="$1" src="$2" want="$3"
    printf '%s\n' "${src}" > "${name}.tk"
    if ! "${TKC}" "${name}.tk" --out "${name}" > "${name}.log" 2>&1; then
        bad "${name}: build failed: $(head -c 300 "${name}.log")"; return; fi
    local got; got="$(./"${name}" 2>&1)"
    if [ "${got}" = "${want}" ]; then ok "${name}: builds, prints '${want}'"
    else bad "${name}: printed '${got}', expected '${want}'"; fi
}

echo "Part 1: escapes are E5001"
refused direct 'm=test;let g=mut."";f=main():i64{{arena g="x\(1)"}<0};' 'str'
refused nested 'm=test;let g=mut."";f=main():i64{{arena if(true){lp(let i=0;i<2;i=i+1){g="k\(i)"}}}<0};' 'str'
refused array 'm=test;let xs=mut.@(1);f=main():i64{{arena xs=@(2;3)}<0};' '@(i64)'
refused map 'm=test;let mp=mut.@("a":1);f=main():i64{{arena mp=@("b":2)}<0};' '@(str:i64)'

echo "Part 2: accepted shapes build and run"
IO='i=io:std.io;'
accepted scalar "m=test;${IO}let n=mut.0;f=main():i64{{arena n=n+41;n=n+1}io.println(\"\\(n)\");<0};" '42'
accepted outer_local "m=test;${IO}f=main():i64{let s=mut.\"\";{arena let t=\"a\\(1)\";s=t}io.println(s);<0};" 'a1'
accepted shadow "m=test;${IO}let g=mut.\"m\";f=main():i64{let g=mut.\"o\";{arena g=\"in\"}io.println(g);<0};" 'in'
accepted semi "m=test;${IO}f=main():i64{let r=mut.0;{arena let x=3;r=x};{arena r=r+1}io.println(\"\\(r)\");<0};" '4'
accepted semi_last "m=test;${IO}f=main():i64{let r=mut.5;if(true){{arena r=r*2};};io.println(\"\\(r)\");<0};" '10'

echo "---------------------------------------------------------------------------"
echo "C2250: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
