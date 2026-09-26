#!/usr/bin/env bash
# C133_metrics_upstream_latency.sh — metrics_record_upstream() records the
# latency it is given.
#
# Story: 135.3f.
#
# THE DEFECT.  src/stdlib/metrics.c's metrics_record_upstream(double
# latency_ms) incremented a counter and never read latency_ms, so a caller
# instrumenting upstream latency got a request COUNT and believed it had
# timings.  The Metrics struct even had an `upstream_sum` field that nothing
# wrote.  Found by the -Werror pass of 135.3a as -Wunused-parameter.
#
# ASSERTED, by value, through the exposition text metrics_prometheus()
# returns (the only way the number leaves the module):
#   * two samples of 250 ms and 1500.5 ms give
#     http_upstream_latency_seconds_sum 1.750500 and _count 2;
#   * a negative latency is counted but adds nothing to the sum;
#   * http_upstream_requests_total still counts every call.
# The harness is compiled against src/stdlib/metrics.c directly, at the gate's
# warning level; C133_SRC overrides the source (used for the negative control
# against the pre-fix file).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC="${C133_SRC:-${REPO_ROOT}/src/stdlib/metrics.c}"
CC="${CC:-cc}"

PASS=0
FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c133_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C133: metrics_record_upstream records latency (135.3f)"
echo "--------------------------------------------------"

cat > "${WORK}/h.c" <<'CEOF'
#include <stdio.h>
#include <stdlib.h>
void metrics_record_upstream(double latency_ms);
char *metrics_prometheus(void);
int main(void)
{
    metrics_record_upstream(250.0);
    metrics_record_upstream(1500.5);
    metrics_record_upstream(-3.0);
    char *p = metrics_prometheus();
    if (!p) return 3;
    fputs(p, stdout);
    free(p);
    return 0;
}
CEOF

if ! "${CC}" -std=c99 -D_GNU_SOURCE -o "${WORK}/h" "${WORK}/h.c" "${SRC}" \
        >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: harness did not build"; sed 's/^/    /' "${WORK}/cc.log" | head -5
    echo "C133: 0 passed, 1 failed"; exit 1
fi
if ! "${WORK}/h" > "${WORK}/out.txt" 2>&1; then
    echo "  FAIL: metrics_prometheus() returned NULL or the harness crashed"
    echo "C133: 0 passed, 1 failed"; exit 1
fi

# value NAME -> the sample value on the line starting with NAME, via awk (a
# missing line prints the sentinel MISSING, never an empty string).
value() { awk -v n="$1" '$1==n{v=$2} END{print (v=="" ? "MISSING" : v)}' "${WORK}/out.txt"; }

check() {
    local what="$1" got="$2" want="$3"
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: ${what} = ${got}"; PASS=$((PASS+1))
    else
        echo "  FAIL: ${what}: got '${got}', want '${want}'"; FAIL=$((FAIL+1))
    fi
}

check "http_upstream_latency_seconds_sum"   "$(value http_upstream_latency_seconds_sum)"   "1.750500"
check "http_upstream_latency_seconds_count" "$(value http_upstream_latency_seconds_count)" "3"
check "http_upstream_requests_total"        "$(value http_upstream_requests_total)"        "3"

echo "C133: ${PASS} passed, ${FAIL} failed"
# Positive assertion: exactly three checks ran.
[ $((PASS+FAIL)) -eq 3 ] || { echo "C133: expected 3 checks, ran $((PASS+FAIL))"; exit 1; }
[ "${FAIL}" -eq 0 ]
