#!/usr/bin/env bash
# C106_glue_zero_false_float_assert.sh — the three glue defects of story
# 136.30, each pinned by running a real program.
#
# 1. SERIALISATION ACCESSORS AND A LEGITIMATE ZERO.  toon_glue.c (136.16) and
#    yaml_glue.c (136.32) keep tk_current_error truthful and return the real
#    value, but the match site tested the VALUE unless the wrapper was listed
#    in is_num_parse_wrapper() (src/llvm.c).  So `n: 0`, `b: false` and
#    `x: 0.0` took the $err arm: a key that says false read as "absent", and a
#    caller substituted its default.  Measured at d1f1ba6: all six of
#    toon.i64/bool/f64 and yaml.i64/bool/f64 on a zero took $err.  127.67 fixed
#    exactly this for std.toml; 136.30 applies the same remedy.  0.0 belongs
#    too: its bit pattern IS the 0 sentinel.
#
# 2. FLOAT RETURNS FROM MODULES OFF A HAND-WRITTEN LIST.  136.25 found
#    is_f64_returning_wrapper consulted .tki return types only for a
#    hardcoded module list without ml, chart, html, llm, test or dataframe, so
#    ml.accuracy printed 4604930618986332160 for 0.75.  127.80 (8934347)
#    replaced the list with a directory scan, which fixed it; this pins it.
#
# 3. test.assert NEVER MARKED A FAILURE.  tk_test_assert_w reimplemented the
#    check instead of calling tk_test_assert, so a false assertion printed a
#    line but was never marked: inside test.run the test was PASS and
#    test.report exited 0.  The family of 127.68 / 134.17.
#
# Negative controls: remove the six tk_toon_* / tk_yaml_* names from
# is_num_parse_wrapper (section 1 goes red); restore the old body of
# tk_test_assert_w (section 3 goes red).
#
# Story: 136.30

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c106_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

# build NAME SOURCE -> ${WORK}/NAME.bin, or a counted failure.
build() {
    printf '%s\n' "$2" > "${WORK}/$1.tk"
    if ! "${TKC}" --allow-all --out "${WORK}/$1.bin" "${WORK}/$1.tk" > "${WORK}/$1.build" 2>&1; then
        echo "  FAIL: $1 did not build"
        sed 's/^/        /' "${WORK}/$1.build" | head -4
        FAIL=$((FAIL + 1))
        return 1
    fi
}

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

echo "C106: 136.30 — zero/false accessors, f64 returns, test.assert"
echo "--------------------------------------------------------------"
echo ""
echo "1. a zero, false or 0.0 is a value, not an error"

# Each accessor falls back to a non-zero default (7, true, 0.5) on $err, so a
# zero that wrongly takes $err prints the default.  A missing key must still
# take $err, so the fix cannot have simply disabled the check.
# (Values are bound with let and printed afterwards: `v as $str` on an f64
# inside a match arm is a separate codegen defect, E9003 on main too.)
if build zero 'm=t;
i=toon:std.toon;
i=yaml:std.yaml;
i=io:std.io;
f=main():i64{
  let t=mt toon.dec("r[1]{n,b,x}:\n0|false|0.0\n") {$ok:v v;$err:e <1};
  let tn=mt toon.i64(t;"n") {$ok:v v;$err:e 7};
  let tb=mt toon.bool(t;"b") {$ok:v v;$err:e true};
  let tx=mt toon.f64(t;"x") {$ok:v v;$err:e 0.5};
  let tm=mt toon.i64(t;"nosuch") {$ok:v v;$err:e 7};
  io.println("\(tn) \(tb as $str) \(tx) \(tm)");
  let y=mt yaml.dec("n: 0\nb: false\nx: 0.0\n") {$ok:v v;$err:e <1};
  let yn=mt yaml.i64(y;"n") {$ok:v v;$err:e 7};
  let yb=mt yaml.bool(y;"b") {$ok:v v;$err:e true};
  let yx=mt yaml.f64(y;"x") {$ok:v v;$err:e 0.5};
  let ym=mt yaml.i64(y;"nosuch") {$ok:v v;$err:e 7};
  io.println("\(yn) \(yb as $str) \(yx) \(ym)");
  <0
};'; then
    out="$("${WORK}/zero.bin" 2>/dev/null)"
    expect "toon.i64/bool/f64 of 0/false/0.0 take \$ok; a missing key takes \$err" \
        "$(echo "${out}" | sed -n 1p)" "0 0 0 7"
    expect "yaml.i64/bool/f64 of 0/false/0.0 take \$ok; a missing key takes \$err" \
        "$(echo "${out}" | sed -n 2p)" "0 0 0 7"
fi

echo ""
echo "2. a .tki f64 return is carried as f64 for ml (136.25's example)"

if build mlacc 'm=t;
i=ml:std.ml;
i=io:std.io;
f=main():i64{
  let a=ml.accuracy(@(1;0;1;1);@(1;0;0;1));
  io.println("\(a)");
  io.println("\(a*2.0)");
  <0
};'; then
    out="$("${WORK}/mlacc.bin" 2>/dev/null | tr '\n' ' ' | sed 's/ *$//')"
    expect "ml.accuracy prints 0.75 and doubles as a float, not its bit pattern" \
        "${out}" "0.75 1.5"
fi

echo ""
echo "3. test.assert marks the test failed"

if build tfail 'm=t;
i=test:std.test;
i=io:std.io;
f=body():i64{
  test.assert(false;"widget count");
  <0
};
f=main():i64{
  test.run(body);
  test.report(0);
  io.println("report returned");
  <0
};'; then
    rc=0; "${WORK}/tfail.bin" > "${WORK}/tfail.out" 2> "${WORK}/tfail.err" || rc=$?
    expect "a failing test.assert inside test.run is reported FAIL" \
        "$(grep -a -c '^FAIL \[' "${WORK}/tfail.err")" "1"
    expect "it emits one structured assertion_failed line carrying the message" \
        "$(grep -a -c 'kind=assertion_failed msg="widget count"' "${WORK}/tfail.err")" "1"
    expect "test.report exits 1" "${rc}" "1"
fi

if build tpass 'm=t;
i=test:std.test;
i=io:std.io;
f=body():i64{
  test.assert(true;"widget count");
  <0
};
f=main():i64{
  test.run(body);
  test.report(0);
  io.println("report returned");
  <0
};'; then
    rc=0; "${WORK}/tpass.bin" > "${WORK}/tpass.out" 2> "${WORK}/tpass.err" || rc=$?
    expect "a passing test.assert is PASS, silent, and test.report returns" \
        "$(grep -a -c '^PASS \[' "${WORK}/tpass.err") $(grep -a -c assertion_failed "${WORK}/tpass.err") ${rc} $(cat "${WORK}/tpass.out")" \
        "1 0 0 report returned"
fi

echo "--------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
