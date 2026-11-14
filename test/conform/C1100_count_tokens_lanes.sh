#!/usr/bin/env bash
# C1100 -- the 131.50 token lanes: superword8k, bytepatch and o200k in
# scripts/patterns/count_tokens.py, each carrying the TEMSpec §6.3 fields.
#
# Runs scripts/patterns/tests/test_lanes.py (plus the count_tokens region
# tests it builds on).  What it pins:
#   - the superword trainer forms superwords (merges across `=`/`(`/`;`) and
#     its encoder reproduces the trainer's segmentation exactly;
#   - the byte-patch threshold is calibrated to the target mean patch and
#     survives a save/load round trip;
#   - `tokens` keeps exactly the five catalogue keys; lanes live under
#     `lanes`, and a missing artefact or an uncached o200k is a named SKIP
#     that says how to get it -- never a network call, never a silent zero;
#   - the `--temspec` report carries metric type, tokenizer, baseline and N
#     on every comparison and passes scripts/check_metrics_claims.py, while
#     the same comparison as a bare percentage is rejected (negative control).
#
# Story: 131.50
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
# 127.439: TKC_PY names the interpreter, as for C031/C032: CI installs pytest
# and tokenizers into a virtualenv (a system pip install is refused under
# PEP 668), and the plain python3 on PATH never sees them.
PY="${TKC_PY:-python3}"
command -v "${PY}" >/dev/null 2>&1 || { echo "CONFORM-SKIP: ${PY} not found"; exit 0; }
"${PY}" -c 'import pytest' 2>/dev/null || { echo "CONFORM-SKIP: pytest not installed (python3 -m pip install pytest, or set TKC_PY to a python that has it)"; exit 0; }
"${PY}" -c 'import tokenizers' 2>/dev/null || { echo "CONFORM-SKIP: tokenizers not installed (python3 -m pip install tokenizers, or set TKC_PY to a python that has it)"; exit 0; }
cd "${REPO_ROOT}" || exit 1
OUT="$("${PY}" -m pytest -q -p no:cacheprovider scripts/patterns/tests/test_lanes.py \
       scripts/patterns/tests/test_count_tokens.py 2>&1)"
RC=$?
echo "${OUT}" | grep -E 'passed|failed|error|skipped' | tail -3
if [ ${RC} -ne 0 ]; then echo "${OUT}" | tail -40; echo "FAIL: C1100"; exit 1; fi
if echo "${OUT}" | grep -q ' skipped'; then echo "FAIL: C1100 -- a lane test skipped (tkc or tokenizers missing?)"; exit 1; fi
echo "PASS: C1100 count_tokens lanes"
