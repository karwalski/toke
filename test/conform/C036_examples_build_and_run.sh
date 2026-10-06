#!/usr/bin/env bash
# C036_examples_build_and_run.sh — the CLI examples in examples/ build with
# the commands their READMEs give, and print what their READMEs say.
#
# Nothing ran the examples, so they rotted unseen: examples/mortgage could not
# be built at all (the multi-module link regression, C035), its build.sh
# defaulted the stdlib to one developer's home directory, datapipe's README
# described a one-argument CLI the program no longer has, and neither
# datapipe's nor tkgrep's README mentioned the capability flags without which
# they stop at CAP001.
#
# Expected values are the READMEs' own figures; the mortgage figures also
# agree with the closed-form annuity formula (500000 at 6.5% over 360 months).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
EX="${REPO_ROOT}/examples"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_examples_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "C036: the examples build and print their documented output"
echo "--------------------------------------"

# check NAME EXPECTED ACTUAL
check() {
    if [ "$2" = "$3" ]; then
        echo "  PASS: $1"; PASS=$((PASS + 1))
    else
        echo "  FAIL: $1"
        echo "      expected: $(printf '%s' "$2" | head -c 400)"
        echo "      got:      $(printf '%s' "$3" | head -c 400)"
        FAIL=$((FAIL + 1))
    fi
}

# build NAME OUT FILE...   (compiles; records a FAIL and returns 1 on error)
build() {
    local name="$1" out="$2"; shift 2
    if "${TKC}" "$@" --out "${out}" >"${WORK}/${name}.log" 2>&1 && [ -x "${out}" ]; then
        return 0
    fi
    echo "  FAIL: ${name}: build failed"
    grep -v '"severity":"warning"' "${WORK}/${name}.log" | sed 's/^/      /' | head -10
    FAIL=$((FAIL + 1))
    return 1
}

# ── mortgage: three modules, built by its own build.sh (TKC pinned) ────────
if TKC="${TKC}" bash "${EX}/mortgage/build.sh" "${WORK}/mortgage" >"${WORK}/mortgage.log" 2>&1 \
   && [ -x "${WORK}/mortgage" ]; then
    out="$(printf '500000\n0.065\n30\n0\n' | "${WORK}/mortgage" 2>&1)"
    check "mortgage monthly payment" "Monthly payment:  3160.34" \
          "$(printf '%s\n' "${out}" | grep '^Monthly payment:')"
    check "mortgage total interest"  "Total interest:   637722.44" \
          "$(printf '%s\n' "${out}" | grep '^Total interest:')"
    check "mortgage first row"       "1      3160.34    452.01    2708.33    499547.99" \
          "$(printf '%s\n' "${out}" | grep '^1 ')"
    check "mortgage amortises to 0"  "0.00" \
          "$(printf '%s\n' "${out}" | grep '^360 ' | awk '{print $NF}')"
else
    echo "  FAIL: mortgage: build.sh failed"
    grep -v '"severity":"warning"' "${WORK}/mortgage.log" | sed 's/^/      /' | head -10
    FAIL=$((FAIL + 1))
fi

# ── tkgrep ─────────────────────────────────────────────────────────────────
printf 'apple pie\nBanana split\napple tart\ncherry\n' > "${WORK}/f.txt"
if build tkgrep "${WORK}/tkgrep" "${EX}/tkgrep/main.tk"; then
    G="${WORK}/tkgrep"
    check "tkgrep match"        $'1:apple pie\n3:apple tart' "$("${G}" --allow-read apple "${WORK}/f.txt")"
    check "tkgrep -i"           "2:Banana split"            "$("${G}" --allow-read -i BANANA "${WORK}/f.txt")"
    check "tkgrep -c"           "2"                         "$("${G}" --allow-read -c apple "${WORK}/f.txt")"
    check "tkgrep -n"           $'apple pie\napple tart'    "$("${G}" --allow-read -n apple "${WORK}/f.txt")"
    # A trailing newline is not an extra, empty line (it was printed as "5:").
    check "tkgrep -v"           $'2:Banana split\n4:cherry' "$("${G}" --allow-read -v apple "${WORK}/f.txt")"
    check "tkgrep -v -c"        "2"                         "$("${G}" --allow-read -v -c apple "${WORK}/f.txt")"
    rc=0; "${G}" --allow-read zzz "${WORK}/f.txt" >/dev/null 2>&1 || rc=$?
    check "tkgrep no match exits 1" "1" "${rc}"
    rc=0; out="$("${G}" apple "${WORK}/f.txt" 2>&1)" || rc=$?
    check "tkgrep without --allow-read is CAP001" "1 CAP001" \
          "${rc} $(printf '%s' "${out}" | grep -o '^CAP001' | head -1)"
fi

# ── datapipe ───────────────────────────────────────────────────────────────
printf 'item,price,qty\nwidget,9.99,100\ngadget,19.50,50\nbolt,4.25,200\n' > "${WORK}/sales.csv"
if build datapipe "${WORK}/datapipe" "${EX}/datapipe/main.tk"; then
    ( cd "${WORK}" && ./datapipe --allow-read --allow-write sales.csv report.json ) \
        > "${WORK}/dp.out" 2>&1
    # The README's example block, verbatim, is the expected stdout.
    sed -n '/^\$ \.\/datapipe --allow-read --allow-write sales.csv/,/^```/p' \
        "${EX}/datapipe/README.md" | sed '1d;$d' > "${WORK}/dp.expected"
    check "datapipe stdout matches README" "$(cat "${WORK}/dp.expected")" "$(cat "${WORK}/dp.out")"
    check "datapipe JSON report" \
          '{"filename":"sales.csv","rows":3,"columns":3,"stats":[{"name":"price","count":3,"sum":33.74,"min":4.25,"max":19.50,"avg":11.25},{"name":"qty","count":3,"sum":350.00,"min":50.00,"max":200.00,"avg":116.67}]}' \
          "$(cat "${WORK}/report.json" 2>/dev/null)"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
