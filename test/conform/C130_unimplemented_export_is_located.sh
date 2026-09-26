#!/usr/bin/env bash
# C130_unimplemented_export_is_located.sh — a call to a stdlib export that has
# no implementation is reported AT THE CALL, by name (story 127.218).
#
# THE DEFECT.  An export on scripts/check_tki_skiplist.txt is declared in its
# .tki but no C source defines its glue symbol.  A call to it type-checks,
# and the build then failed at `ld` with a bare mangled symbol, reported as
# E9003 "undefined symbol 'tk_canvas_fill_w' — while compiling f=main" at the
# enclosing declaration's line.  Codegen now records each stdlib call site by
# the glue symbol it lowered to; when ld names that symbol undefined, the
# failure is E4027 at the call, naming `cv.fill` and std.canvas.  It is
# consulted only after the link has failed, so it cannot reject a program
# that links.
#
# Also from the row: `x as str` on an f64 (and so `v as $str` in a match arm
# over `toon.f64`) returned the raw i64 from tk_str_fromfloat_w where the
# expression is typed i8*, and every use was invalid IR (E9003).
#
# WHAT THIS DRIVES.  (1) the canvas.fill call: exit non-zero, exactly one
# error, E4027, at line 6 col 3, message naming 'cv.fill'; no E9003.
# (2) `x as str` for f64 and f32, and inside a match arm, build and print.
#
# Negative control: remove the `if (uc) { ... }` block in compile_binary_multi
# — case 1 reports E9003 at line 4 (f=main) and fails.
#
# Story: 127.218

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c130_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C130: an unimplemented stdlib export is reported at the call"
echo "------------------------------------------------------------"

# The case depends on canvas.fill still being quarantined.  If it has been
# implemented, this test must be pointed at another quarantined export.
if ! awk '$1=="canvas.tki::canvas.fill"{f=1} END{exit !f}' "${REPO_ROOT}/scripts/check_tki_skiplist.txt"; then
    fail "canvas.fill is no longer on scripts/check_tki_skiplist.txt — re-point case 1 at a quarantined export"
else
    cat > "${WORK}/q.tk" <<'EOF'
m=q;
i=io:std.io;
i=cv:std.canvas;
f=main():i64{
  let c=cv.new("x";10;10);
  cv.fill(c);
  <0
};
EOF
    "${TKC}" --allow-all --out "${WORK}/q" "${WORK}/q.tk" >"${WORK}/q.log" 2>&1; rc=$?
    diags="$(awk '/"severity":"error"/' "${WORK}/q.log")"
    nerr="$(printf '%s\n' "${diags}" | awk 'NF' | wc -l | tr -d ' ')"
    if [ "${rc}" != 0 ] && [ "${nerr}" = 1 ] &&
       printf '%s' "${diags}" | awk '/"error_code":"E4027"/ && /"line":6,"col":3/ && /cv\.fill/ {f=1} END{exit !f}'; then
        pass "cv.fill: exit ${rc}, one error, E4027 at 6:3 naming cv.fill"
    else
        fail "cv.fill: exit ${rc}, ${nerr} error(s): $(printf '%s' "${diags}" | cut -c1-300)"
    fi
fi

cat > "${WORK}/f.tk" <<'EOF'
m=fstr;
i=io:std.io;
t=$bad{code:i64};
f=half(x:f64):f64!$bad{if(x<0.0){<$bad{code:1}};<x/2.0};
f=main():i64{
  let x=2.5;
  io.println(x as str);
  let y:f32=0.25 as f32;
  let s=y as str;
  io.println(s);
  let m=mt half(5.0){$ok:v v as $str;$err:e "neg"};
  io.println(m);
  <0
};
EOF
for o in -O0 -O2; do
    if "${TKC}" --allow-all "${o}" --out "${WORK}/f" "${WORK}/f.tk" >"${WORK}/f.log" 2>&1; then
        got="$("${WORK}/f" </dev/null | tr '\n' '|')"
        if [ "${got}" = "2.5|0.25|2.5|" ]; then pass "float as str ${o}: ${got}"
        else fail "float as str ${o}: got [${got}] want [2.5|0.25|2.5|]"; fi
    else
        fail "float as str ${o}: did not build: $(awk '/"severity":"error"/' "${WORK}/f.log" | head -1 | cut -c1-240)"
    fi
done

echo ""
echo "C130: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
