#!/usr/bin/env bash
# C109_chart_bar_lifetime_asan.sh — chart.bar's spec owns what it points at,
# checked by AddressSanitizer rather than by luck (story 136.31).
#
# THE DEFECT (fixed in 136384e, 136.22; recorded as 136.31).  make_spec() in
# src/stdlib/chart.c stores the pointers it is handed and copies nothing.  The
# old tk_chart_bar_w passed it the address of a STACK TkDataset and then
# free()d the labels block and the values block it had just built, so the
# spec it returned pointed at three dead objects and chart.tojson read them.
# That can print a plausible chart rather than crash, which is why a
# behavioural test (test/stdlib/glue_contract.sh, chart_title.tk) is not
# enough on its own: this test runs the wrapper and the serialiser under ASan.
#
# WHAT IS ASSERTED.
#   1. Detector control, run FIRST: the pre-136.22 body of tk_chart_bar_w,
#      embedded below verbatim (plus the title parameter it lacked, ignored),
#      built with the same harness and flags, MUST be reported by ASan as a
#      use-after-free / use-after-return.  Without this, "no finding" and
#      "the instrumentation never ran" look the same.
#   2. The shipped src/stdlib/chart_glue.c under ASan+UBSan (135.30: its
#      result is the $chartspec record, read through chart_spec_from_rec as
#      chart.tojson reads it -- casting it to TkChartSpec * is the layout
#      confusion 135.30 removed): clean exit, no
#      sanitizer report, and chart_tojson carries the labels, the values and
#      the title after other allocations have churned the heap and stack.
#
# The sibling defect 136.31 records — tk_html_doc_w DEFINED with a parameter
# while declared with none, so it read an uninitialised register — is a
# signature disagreement, which ASan cannot see.  Its gate is structural:
# src/stdlib_decls_gen.h is generated from the C definitions, and
# `make check-tki` compares every .tki arity against it (html.doc: 0).
# Measured: redefining tk_html_doc_w to take and read an int64_t makes
# check-tki fail on "html.doc ... .tki declares 0, g_stdlib_decls declares 1".
# (A parameter the body never reads is the 127.61 zero-argument exemption.)
#
# Story: 136.31

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SL="${REPO_ROOT}/src/stdlib"
CC="${CC:-cc}"

PASS=0
FAIL=0

WORK="$(mktemp -d /tmp/tkc_c109_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "C109: chart.bar's spec survives its wrapper (ASan)"
echo "--------------------------------------------------"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -O1 -g"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"

# A toke array is an i64 block whose word [-1] is the count.
cat > "${WORK}/harness.c" <<'C'
#include "chart.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int64_t tk_chart_bar_w(int64_t labels, int64_t data, int64_t title);
static int64_t arr(int64_t n) {
    int64_t *b = calloc((size_t)n + 1, sizeof(int64_t));
    b[0] = n;
    return (int64_t)(intptr_t)(b + 1);
}
/* Reuse the stack the wrapper used and the heap blocks it may have freed. */
static void churn(void) {
    volatile char pad[512];
    memset((char *)pad, 'X', sizeof pad);
    for (int i = 0; i < 64; i++) {
        void *p = malloc(16 * (size_t)(i % 8 + 1));
        memset(p, 'Y', 16 * (size_t)(i % 8 + 1));
        free(p);
    }
}
static int64_t build(void) {
    int64_t l = arr(2), d = arr(2);
    int64_t *lp = (int64_t *)(intptr_t)l, *dp = (int64_t *)(intptr_t)d;
    lp[0] = (int64_t)(intptr_t)"Q1";
    lp[1] = (int64_t)(intptr_t)"Q2";
    double a = 1.5, b = 2.5;
    memcpy(&dp[0], &a, 8);
    memcpy(&dp[1], &b, 8);
    return tk_chart_bar_w(l, d, (int64_t)(intptr_t)"Revenue");
}
int main(void) {
    int64_t spec = build();
    churn();
#ifdef SPEC_IS_REC
    /* 135.30: tk_chart_bar_w returns the $chartspec record; chart.tojson
     * (tk_chart_tojson_w) converts it exactly like this before serialising */
    TkChartSpec *s = chart_spec_from_rec(spec);
#else
    TkChartSpec *s = (TkChartSpec *)(intptr_t)spec;   /* pre-135.30 bodies */
#endif
    const char *j = chart_tojson(s);
    printf("%s\n", j ? j : "(null)");
    return 0;
}
C

# The pre-136.22 body (136384e^:src/stdlib/tk_web_glue.c), verbatim but for
# the third parameter the harness passes.
cat > "${WORK}/old_bar.c" <<'C'
#include "chart.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
int64_t tk_chart_bar_w(int64_t labels_i64, int64_t data_i64, int64_t title_i64) {
    (void)title_i64;
    StrArray labels = { NULL, 0 };
    if (labels_i64) {
        int64_t *lp = (int64_t *)(intptr_t)labels_i64;
        int64_t n = lp[-1];
        if (n > 0) {
            labels.data = (const char **)malloc((size_t)n * sizeof(const char *));
            if (labels.data) {
                labels.len = (uint64_t)n;
                for (int64_t i = 0; i < n; i++)
                    labels.data[i] = (const char *)(intptr_t)lp[i];
            }
        }
    }
    TkDataset ds;
    memset(&ds, 0, sizeof(ds));
    ds.label = "data";
    ds.color = NULL;
    if (data_i64) {
        int64_t *dp = (int64_t *)(intptr_t)data_i64;
        int64_t n = dp[-1];
        if (n > 0) {
            double *vals = (double *)malloc((size_t)n * sizeof(double));
            if (vals) {
                for (int64_t i = 0; i < n; i++)
                    memcpy(&vals[i], &dp[i], sizeof(double));
                ds.values = vals;
                ds.nvalues = (uint64_t)n;
            }
        }
    }
    TkChartSpec *spec = chart_bar(labels, &ds, 1, NULL);
    free((void *)labels.data);
    free((void *)ds.values);
    return (int64_t)(intptr_t)spec;
}
C

# ── 1. detector control ────────────────────────────────────────────────
if ! ${CC} ${SAN} -I"${SL}" -o "${WORK}/old" "${WORK}/harness.c" \
        "${WORK}/old_bar.c" "${SL}/chart.c" > "${WORK}/old.build" 2>&1; then
    echo "  FAIL: the ASan control did not build"
    sed 's/^/        /' "${WORK}/old.build" | head -6
    FAIL=$((FAIL + 1))
else
    "${WORK}/old" > "${WORK}/old.out" 2> "${WORK}/old.err"; rc=$?
    if [ "${rc}" -ne 0 ] && grep -a -q -E \
         'ERROR: AddressSanitizer: (heap-use-after-free|stack-use-after-return|stack-use-after-scope)' \
         "${WORK}/old.err"; then
        echo "  PASS: control: ASan reports the pre-136.22 body ($(grep -a -o -m1 -E 'AddressSanitizer: [a-z-]+' "${WORK}/old.err"))"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: control: ASan did NOT flag the pre-136.22 body (exit ${rc});"
        echo "        refusing to trust a clean result below"
        head -5 "${WORK}/old.err" | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

# ── 2. the shipped wrapper ─────────────────────────────────────────────
if ! ${CC} ${SAN} -DSPEC_IS_REC -I"${SL}" -o "${WORK}/new" "${WORK}/harness.c" \
        "${SL}/chart_glue.c" "${SL}/chart.c" > "${WORK}/new.build" 2>&1; then
    echo "  FAIL: the shipped chart_glue.c did not build under ASan"
    sed 's/^/        /' "${WORK}/new.build" | head -6
    FAIL=$((FAIL + 1))
else
    "${WORK}/new" > "${WORK}/new.out" 2> "${WORK}/new.err"; rc=$?
    if [ "${rc}" -eq 0 ] && ! grep -a -q -E 'AddressSanitizer|runtime error:' "${WORK}/new.err"; then
        echo "  PASS: shipped tk_chart_bar_w + chart_tojson run clean under ASan+UBSan"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: shipped wrapper: exit ${rc}, sanitizer output:"
        head -8 "${WORK}/new.err" | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
    j="$(cat "${WORK}/new.out")"
    ok=1
    for want in '"Q1"' '"Q2"' '1.5' '2.5' '"Revenue"'; do
        case "${j}" in *"${want}"*) ;; *) ok=0; echo "        missing ${want}";; esac
    done
    if [ "${ok}" -eq 1 ]; then
        echo "  PASS: the JSON still carries labels, values and title after churn"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: the JSON lost data: ${j}"
        FAIL=$((FAIL + 1))
    fi
fi

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
