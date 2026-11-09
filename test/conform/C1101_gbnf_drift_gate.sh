#!/usr/bin/env bash
# C1101 -- docs/spec/toke.gbnf agrees with the real parser (131.52).
#
# `make check-gbnf` (scripts/gbnf_gate.py) asserts, over the committed fragment
# set test/gbnf/fragments.json (conformance inputs, standalone tests, examples,
# the tree-sitter corpus, test/gbnf/extra and single-edit mutants of each),
#     the GBNF accepts F  <=>  `tkc --dump-ast F` parses F cleanly,
# apart from the documented KNOWN over-approximations, each pinned by id.
#
# Negative controls (run on the extra/ fragments, a quick subset), each a
# one-line edit to a temp copy of the grammar that the gate must catch:
#   1. `"let" ws1 ident` -> `"let" ws ident`: `letx=1` lexes as one name, so
#      the grammar now accepts programs the parser rejects (GBNF-ONLY);
#   2. drop the `0b...` binary literal: the parser accepts programs the
#      grammar rejects (PARSER-ONLY);
#   3. append a left-recursive rule: llama.cpp's GBNF engine loops on it.
#
# Story: 131.52
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
export TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
cd "${REPO_ROOT}" || exit 1
PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

python3 scripts/gbnf_gate.py > "${WORK}/gate.log" 2>&1; rc=$?
tail -2 "${WORK}/gate.log"
if [ ${rc} -eq 0 ] && grep -q '^\[gate\] PASS' "${WORK}/gate.log"; then ok "gate: grammar <=> parser on the full fragment set"
else bad "gate"; cat "${WORK}/gate.log"; fi

control() {   # $1 name  $2 python edit  $3 expected marker in the gate output
    local g="${WORK}/$1.gbnf"
    python3 - "docs/spec/toke.gbnf" "$g" "$2" <<'PY' || { bad "$1: injection did not apply"; return; }
import sys
src, dst, edit = sys.argv[1], sys.argv[2], sys.argv[3]
t = open(src).read()
old, new = edit.split("=>", 1)
if old == "APPEND":
    t2 = t + new.replace("\\n", "\n")
else:
    if old not in t:
        sys.exit(1)
    t2 = t.replace(old, new, 1)
assert t2 != t
open(dst, "w").write(t2)
PY
    python3 scripts/gbnf_gate.py --gbnf "$g" --only extra/ > "${WORK}/$1.log" 2>&1; local r=$?
    if [ $r -ne 0 ] && grep -q "$3" "${WORK}/$1.log"; then ok "negative control $1 caught ($3)"
    else bad "negative control $1 not caught"; tail -5 "${WORK}/$1.log"; fi
}
control let-boundary '"let" ws1 ident (ws=>"let" ws ident (ws' 'GBNF-ONLY'
control no-binary '| "0" [bB] [01]+ |=>|' 'PARSER-ONLY'
control left-rec 'APPEND=>\nlr ::= lr "x" | "y"\n' 'left-recursive'

echo "C1101: ${PASS} passed, ${FAIL} failed"
[ ${FAIL} -eq 0 ]
