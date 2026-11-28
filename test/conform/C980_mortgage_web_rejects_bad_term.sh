#!/usr/bin/env bash
# C980_mortgage_web_rejects_bad_term.sh — examples/mortgage-web validates the
# loan term before it becomes unsigned (story 127.328) and bounds it at
# 100 years (story 127.329).
#
# THE DEFECT.  pages/calculate.tk parsed the term as
#   let term=mt str.toint(terms){$ok:v (v as u64); ...};  if(term<1){<bad}
# Once 127.327 made `term` bind the arms' agreed type (u64), "-5" became
# 18446744073709551611, passed `term<1`, and `term*12` hit the multiply
# overflow trap: the server process died on one bad form field.
#
# THE FIX.  The example keeps the parsed term i64, rejects termyears<1, and
# only then casts to u64 for the schedule helpers.
#
# THE BOUND (127.329).  With no upper bound, term=2000000000000000000
# overflowed term*12 (RT002) and killed the server; a huge term below the
# overflow ran the schedule loop term*12 times.  The example now rejects
# termyears>100 with router.bad before the cast.  100 years is the
# example's own product choice, not a language or stdlib limit.
#
# WHAT IS ASSERTED (the real example, built and served on an ephemeral port):
#   1. term=-5  -> 400 "term must be at least 1 year", and the server lives
#   2. term=0   -> 400 "term must be at least 1 year"
#   3. term=abc -> 400 "invalid term"
#   4. term=30  -> 200, monthly $3160.34 (500000 at 6.5%; the C710 figure)
#   5. term=1   -> 200, monthly $43148.21
#   6. term=100 -> 200, monthly $2712.48 (the boundary is accepted)
#   7. term=101 -> 400 "term must be at most 100 years"
#   8. term=2000000000000000000 -> 400 "term must be at most 100 years",
#      and the server lives (before 127.329: RT002, server exits)
#   9. the server is still running after all of the above
#
# Negative control: C980_EXAMPLE=<dir> serves another copy of the example.
# The unfixed calculate.tk (dcf648f0) built with the 127.327 compiler fails
# all six 127.328 cases: the server dies on case 1 and answers nothing
# after it.  The 127.328 example (3b469371) passes 1-6, accepts 101 and
# dies on the huge term, failing 7-9.
#
# Stories: 127.328, 127.329, 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
EXAMPLE="${C980_EXAMPLE:-${REPO_ROOT}/examples/mortgage-web}"
PASS=0; FAIL=0; SRV=""
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
if ! command -v curl >/dev/null 2>&1; then
    echo "SKIP: curl is not available"; echo "Results: 0 passed, 0 failed"; exit 0
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c980_XXXXXX")"
cleanup() { [ -n "${SRV}" ] && { kill -9 "${SRV}" 2>/dev/null; wait "${SRV}" 2>/dev/null; }; rm -rf "${WORK}"; }
trap cleanup EXIT
echo "C980: mortgage-web rejects a negative, zero or over-100-year term (127.328, 127.329)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

# The example as shipped, except it listens on loopback and an ephemeral port.
cp -R "${EXAMPLE}/templates" "${EXAMPLE}/static" "${WORK}/"
mkdir -p "${WORK}/src"
cp "${EXAMPLE}"/pages/*.tk "${WORK}/src/"
sed -i.orig 's/router\.serve(r;"0\.0\.0\.0";8080)/router.serve(r;"127.0.0.1";0)/' "${WORK}/src/app.tk"
if cmp -s "${WORK}/src/app.tk" "${WORK}/src/app.tk.orig"; then
    echo "  FAIL: app.tk no longer serves on \"0.0.0.0\";8080 -- update this test's sed"
    echo "Results: 0 passed, 1 failed"; exit 1
fi
cd "${WORK}"
if ! "${TKC}" src/index.tk src/calculate.tk src/app.tk --out app > build.log 2>&1 || [ ! -x app ]; then
    echo "  FAIL: the example does not build"; grep -v '"severity":"warning"' build.log | sed 's/^/      /' | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

./app --allow-net --allow-fs > srv.out 2> srv.err &
SRV=$!
port=""
tk_wait_until "${SRV}" tk_listening srv.err
port="$(tk_listening_port srv.err)"
if [ -z "${port}" ]; then
    echo "  FAIL: the server did not start"; sed 's/^/      /' srv.err | head -6
    echo "Results: 0 passed, 1 failed"; exit 1
fi

# post TERM -> "<http code> <first line of body, or the monthly payment>"
post() {
    local out code
    out="$(curl -s --max-time 5 -w '\n%{http_code}' \
        -d "principal=500000&rate=0.065&term=$1&extra=0" "http://127.0.0.1:${port}/calculate" 2>/dev/null)"
    code="$(printf '%s' "${out}" | tail -1)"
    if [ "${code}" = 200 ]; then
        echo "${code} $(printf '%s' "${out}" | grep -o '\$[0-9][0-9]*\.[0-9][0-9]' | head -1)"
    else
        echo "${code} $(printf '%s' "${out}" | sed '$d' | head -1)"
    fi
}
alive() { kill -0 "${SRV}" 2>/dev/null && echo alive || echo dead; }

expect "1  term=-5 is rejected and the server lives" "$(post -5) $(alive)" "400 term must be at least 1 year alive"
expect "2  term=0 is rejected"                       "$(post 0)"           "400 term must be at least 1 year"
expect "3  term=abc is rejected"                     "$(post abc)"         "400 invalid term"
expect "4  term=30 computes the documented payment"  "$(post 30)"          "200 \$3160.34"
expect "5  term=1 computes"                          "$(post 1)"           "200 \$43148.21"
expect "6  term=100 (the maximum) computes"            "$(post 100)"         "200 \$2712.48"
expect "7  term=101 is rejected"                     "$(post 101)"         "400 term must be at most 100 years"
expect "8  term=2e18 is rejected and the server lives" "$(post 2000000000000000000) $(alive)" "400 term must be at most 100 years alive"
expect "9  the server is still running"              "$(alive)"            "alive"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
