#!/usr/bin/env bash
# T004_stdlib_link_set.sh — the directory and the manifest must agree on what
# the standard library is (story 127.99).
#
# A rename (136.17) left llmtool.c tracked beside llm_tool.c, and a
# superseded stub of tk_infer_load_streaming in infer.c beside the real
# implementation in infer_stream.c.  `tkc --out` links a curated list — the
# module table in src/stdlib_deps.c, which `tkc --emit-deps` prints — so it
# never named either file and never noticed.  The website deploy globs
# stdlib/*.c on the server, hit multiple definitions at link, and could not
# ship until both were excluded by hand.  Two builds of the same program were
# not building the same thing.
#
# THE MANIFEST IS AUTHORITATIVE; the glob derives from it.  This gate keeps
# the derivation honest, and fails on a tree carrying either stale copy.
#
# Story: 127.99

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"

PASS=0
FAIL=0

echo "T004: src/stdlib/ is a faithful derivation of the link manifest"
echo "--------------------------------------"

OUT="$(python3 "${REPO_ROOT}/scripts/check_stdlib_link_set.py" 2>&1)"
RC=$?
echo "${OUT}" | sed 's/^/  /'

if [ "${RC}" -eq 0 ]; then
    PASS=$(echo "${OUT}" | grep -c '^PASS:')
else
    FAIL=$(echo "${OUT}" | grep -c '^FAIL:')
    [ "${FAIL}" -eq 0 ] && FAIL=1
    PASS=$(echo "${OUT}" | grep -c '^PASS:')
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
