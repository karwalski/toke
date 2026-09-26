#!/usr/bin/env bash
# C185_core_reach_reads_past_comments.sh — check-core-reach sees a public
# function whose doc comment contains `word(` (story 135.19 item 4).
#
# THE DEFECT.  report_core_reach.py matched declarations with a regex that
# spans newlines, over the raw header.  A doc comment such as
# `/* zzq_open -- see also foo(x) */` above `int zzq_open(...);` captured
# `foo` and the declaration itself was never seen: 53 public functions in
# src/stdlib were invisible to the gate that exists to find them.
#
# The fixture is a one-function module in a scratch --src directory; the gate
# must report it UNTRIAGED.
#
# Negative control: scripts/report_core_reach.py from main at 7c16729 — no
# UNTRIAGED line for zzq_open (0).
#
# Story: 135.19

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
REACH="${REACH:-${REPO_ROOT}/scripts/report_core_reach.py}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/stdlib"
printf '/* zzq_open -- see also foo(x) for details */\nint zzq_open(const char *p);\n' > "${WORK}/stdlib/zzq.h"
printf '#include "zzq.h"\nint zzq_open(const char *p){return p!=0;}\n' > "${WORK}/stdlib/zzq.c"
out="$(python3 "${REACH}" --src "${WORK}/stdlib" 2>&1)"
expect "1-commented-declaration-found" "$(printf '%s\n' "${out}" | grep -c '^UNTRIAGED  zzq.open')" "1"
expect "2-comment-word-not-captured"   "$(printf '%s\n' "${out}" | grep -c 'C: foo')" "0"

echo "C185: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
