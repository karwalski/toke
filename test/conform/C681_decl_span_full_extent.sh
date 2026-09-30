#!/usr/bin/env bash
# C681 -- a declaration's --dump-ast span is its full extent (131.31).
#
# A FUNC_DECL's span ended after the `f` keyword, so every consumer
# (scripts/patterns/count_tokens.py, toke-corpus metrics.py, the 131.3
# miner) had to brace-match by hand to find the function's end.  The
# top-level declaration nodes -- MODULE_DECL, IMPORT_DECL, TYPE_DECL,
# CONST_DECL, FUNC_DECL -- now span from their first token to their last
# (a function's closing `}`), less the optional `;` separator.  Every other
# node's span is unchanged: case 6 compares the dump with the declaration
# spans masked against the one-token form.
#
# Negative control: TKC=<437dc471's toke> -- cases 1-5b fail (each span is
# the declaration's first token).
#
# Story: 131.31

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
printf 'm=h;\ni=s:std.str;\nt=$pt{x:i64};\nlim=5:i64;\nf=a():i64{<1};\nf=ext(x:i64):i64;\nf=main():i64{\n  let q=\"}\";\n  <a()\n}\n' > "${WORK}/p.tk"
"${TKC}" --dump-ast "${WORK}/p.tk" > "${WORK}/ast.json" 2>"${WORK}/err" || { echo "FAIL: --dump-ast rc=$?"; cat "${WORK}/err"; exit 1; }

span() {   # the source text of top-level child $1
    python3 - "${WORK}/p.tk" "${WORK}/ast.json" "$1" <<'PY'
import json, sys
src = open(sys.argv[1], "rb").read()
c = json.load(open(sys.argv[2]))["children"][int(sys.argv[3])]
print(c["kind"], repr(src[c["span"]["start"]:c["span"]["end"]].decode()))
PY
}
expect "1 MODULE_DECL spans the declaration" "$(span 0)" "MODULE_DECL 'm=h'"
expect "2 IMPORT_DECL spans the declaration" "$(span 1)" "IMPORT_DECL 'i=s:std.str'"
expect "3 TYPE_DECL spans to its closing brace" "$(span 2)" "TYPE_DECL 't=\$pt{x:i64}'"
expect "4 CONST_DECL spans the declaration" "$(span 3)" "CONST_DECL 'lim=5:i64'"
expect "5 FUNC_DECL spans to its closing brace, not the ';', past a brace in a string" \
    "$(span 6)" "FUNC_DECL 'f=main():i64{\\n  let q=\"}\";\\n  <a()\\n}'"
expect "5b a bodyless FUNC_DECL ends at its return type" "$(span 5)" "FUNC_DECL 'f=ext(x:i64):i64'"

# 6: nothing but the five declaration kinds' top-level spans changed
out=$(python3 - "${WORK}/ast.json" <<'PY'
import json, sys
ast = json.load(open(sys.argv[1]))
DECL = {"MODULE_DECL", "IMPORT_DECL", "TYPE_DECL", "CONST_DECL", "FUNC_DECL"}
bad = []
def walk(n, top):
    s = n["span"]
    if not (top and n["kind"] in DECL):
        # a non-declaration span is one token: it starts at pos.offset
        if s["start"] != n["pos"]["offset"] or s["end"] - s["start"] > 16:
            bad.append((n["kind"], s))
    for c in n.get("children") or []:
        walk(c, n is ast)
walk(ast, False)
print("ok" if not bad else bad[:3])
PY
)
expect "6 every other node keeps its one-token span" "${out}" "ok"

echo "C681: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
