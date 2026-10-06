#!/usr/bin/env bash
# C212_skip_means_exit_zero.sh — no conformance suite prints SKIP and then
# fails (story 127.219 item 3).
#
# THE DEFECT.  127.166 gave the runner a skip: a suite that cannot run for
# want of an optional external tool prints `CONFORM-SKIP: <reason>` and exits
# 0, and scripts/conform_isolated.sh counts and names it.  Many suites still
# printed "SKIP: ..." and exited 1, which the runner reports as a FAILURE
# under a line that says it was a skip -- on main c8575da, 97 suites:
#   * C033 (no OCR engine off macOS; Pillow missing) and V001 (no LLVM
#     verifier): optional tools, so these are skips and now use the protocol;
#   * all 97 printed "SKIP: tkc binary not found" and exited 1.  A missing
#     compiler is not an optional tool; it is a failure and now says FAIL.
#
# THE CHECK.  Static: in every test/conform/*.sh, an `echo "SKIP` at the start
# of a line's command (not an indented per-case "  SKIP", which reports one
# arm of a suite that goes on to run the rest) followed within six lines by
# `exit` with a non-zero status.
#
# EXCEPT exit 77, the runner's PLATFORM skip (PR #1, f21f7432): a suite that
# cannot run on this OS at all (C033 off macOS) prints SKIP and exits 77, and
# scripts/conform_isolated.sh reports it as SKIP by name -- never as a
# failure, even under TKC_CONFORM_NO_SKIP=1.  77 is the protocol, not a FAIL.
#
# Negative control: C212_DIR=<main's test/conform> -- 97 offenders.
#
# Story: 127.219 (item 3)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DIR="${C212_DIR:-${SCRIPT_DIR}}"

echo "C212: a suite that prints SKIP exits 0 (127.219 item 3)"
n=0
for f in "${DIR}"/*.sh; do
    hits="$(awk '
        /echo "SKIP/ && !/CONFORM-SKIP/ && !/echo "  +SKIP/ {
            if ($0 ~ /echo "SKIP.*(^|[;{ ])exit [1-9]/ && $0 !~ /exit 77([^0-9]|$)/) { printf "%d: %s\n", NR, $0; s = 0; next }
            s = NR; l = $0; next }
        s && NR <= s + 6 && /(^|[;{ ])exit 77([^0-9]|$)/ { s = 0 }
        s && NR <= s + 6 && /(^|[;{ ])exit [1-9]/ { printf "%d: %s\n", s, l; s = 0 }
        s && NR > s + 6 { s = 0 }' "${f}")"
    if [ -n "${hits}" ]; then
        n=$((n + 1))
        printf 'FAIL %s\n' "$(basename "${f}")"
        printf '%s\n' "${hits}" | sed 's/^/        /' | cut -c1-160
    fi
done
nsuites="$(ls "${DIR}"/*.sh | wc -l | tr -d ' ')"
[ "${nsuites}" -gt 100 ] || { echo "FAIL only ${nsuites} suites found in ${DIR}"; exit 1; }
if [ "${n}" -eq 0 ]; then
    echo "PASS none of ${nsuites} suites prints SKIP and then exits non-zero"
    echo "C212: 1 passed, 0 failed"; exit 0
fi
echo "C212: 0 passed, 1 failed (${n} suites print SKIP and exit non-zero)"
exit 1
