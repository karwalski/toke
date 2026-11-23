#!/usr/bin/env bash
# C2112_array_lit_opaque_cast.sh — an array literal whose element is a
# binding of an `"opaque": true` std type made with `as` is an array of that
# element, not a spread of it (story 138.60).
#
# THE DEFECT.  `let fe=c as $svgelem; let a=@(fe)` lowered `@(fe)` as a
# spread: the emitter's spread detector (llvm.c array_lit_spread_idx) took
# any i8* pointer local without a registered record layout for an array, and
# an `"opaque": true` .tki type (svgelem, tomlval, csvreader, dataframe, ...)
# has no "fields" key, so none is registered.  The IR read word -1 of the
# element as a length (`getelementptr i64, i64* %fe, i64 -1 ; base.len`):
# len 0 for a real element, SIGSEGV for a forged one.  A type declared
# `"fields": []` ($vechandle) was unaffected.
#
# THE FIX.  An element the checker types as a record is never a spread.
#
# Negative control: TKC=<base 6ec0044d tree>/tkc -- real prints len=0 and an
# empty group; forged exits 139; the IR carries `base.len`.
#
# Story: 138.60
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2112_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2112: @(x as \$opaque) is an array of one element (138.60)"
echo "----------------------------------------------------------------------"

run() {
    local name="$1" want="$2" got rc
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build: $(grep -m1 error "${name}.log" | cut -c1-200)"; return
    fi
    got="$(./"${name}" --allow-all 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "${name}"
    else fail "${name}: rc=${rc}; got:"; printf '%s\n' "${got}" | sed 's/^/      /'; fi
}

cat > real.tk <<'EOF'
m=a;
i=io:std.io;
i=str:std.str;
i=svg:std.svg;
f=main():i64{
  let st=svg.style("red";"blue";1.0);
  let c=svg.circle(1.0;1.0;1.0;st);
  let fe=c as $svgelem;
  let a=@(fe);
  let b=@(fe;c);
  io.println("len=\(a.len) len2=\(b.len)");
  let d=svg.append(svg.doc(10.0;10.0);svg.group(@(fe;c);""));
  let out=svg.render(d);
  io.println("circles=\(str.split(out;"<circle").len - 1)");
  <0
};
EOF
run real 'len=1 len2=2
circles=2'

cat > forged.tk <<'EOF'
m=a;
i=io:std.io;
i=svg:std.svg;
f=main():i64{
  let n=5;
  let fe=n as $svgelem;
  let a=@(fe);
  io.println("len=\(a.len)");
  let g=svg.group(@(fe);"");
  io.println("group answered");
  <0
};
EOF
run forged 'len=1
group answered'

# The IR: no spread of the element (no `base.len` read off it).
if "${TKC}" --emit-llvm real.tk -o real.ll > ll.log 2>&1; then
    if grep -q 'base.len' real.ll; then fail "IR reads word -1 of the element (base.len)"
    else pass "IR has no spread of the element"; fi
else
    fail "--emit-llvm: $(head -c 200 ll.log)"
fi

echo "----------------------------------------------------------------------"
echo "C2112: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
