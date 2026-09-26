#!/usr/bin/env bash
# C128_function_values.sh — calling a function value (story 124.0c).
#
# THE DEFECTS.
#   1. `let g=&add; g(1;2)` passed --check and failed in clang with
#      "use of undefined value '@g'": codegen emitted the local's name as a
#      global function symbol.  It is now an indirect call through the
#      address the local holds, typed from the referenced function's
#      signature (the same lookup the reference itself uses).
#   2. (row item 4) closure captures travelled as i64 slots and were
#      reloaded as i64, so an f64 capture was `sitofp`'d from its bit
#      pattern (`x*k` with k=2.5 gave 9.2e18); closure parameters were
#      spilled untagged, so an `@f64` parameter read its elements as
#      integers; and a closure call's result type was answered as i64 for
#      any closure, so an f64 result was invalid IR.
#   3. (row item 3, partial) the env box of a closure literal applied on the
#      spot has one reference, the call; it is now freed after it.  A
#      closure bound to a local is still never freed — that needs the
#      reference tracking of memory-model §2.5 and is NOT covered here.
#
# WHAT THIS DRIVES.  Each case builds and runs at -O0 and -O2 and must print
# its hand-derived line; case 5 also asserts the emitted IR frees the env.
#
# Negative control: delete the `fref_lcount` call path in emit_expr — case 1
# fails to build ("use of undefined value '@g'"); revert the capture-type
# restore in emit_lifted_closure — case 2 prints 9.22562e+18.
#
# Story: 124.0c

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

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c128_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C128: function values — references, closure captures and parameters"
echo "--------------------------------------------------------------------"

# case NAME WANT BODY  (BODY is main's statements; helpers are always declared)
case_() {
    local name="$1" want="$2" body="$3" o
    cat > "${WORK}/${name}.tk" <<EOF
m=c128;
i=io:std.io;
f=add(a:i64;b:i64):i64{<a+b};
f=sub(a:i64;b:i64):i64{<a-b};
f=half(x:f64):f64{<x/2.0};
f=hi(s:str):str{<"hi \(s)"};
f=main():i64{
${body}
  <0
};
EOF
    if ! "${TKC}" --check "${WORK}/${name}.tk" >"${WORK}/${name}.chk" 2>&1; then
        fail "${name}: --check rejected it: $(head -c 200 "${WORK}/${name}.chk")"; return
    fi
    for o in -O0 -O2; do
        if ! "${TKC}" --allow-all "${o}" --out "${WORK}/${name}" "${WORK}/${name}.tk" >"${WORK}/${name}.b" 2>&1; then
            fail "${name} ${o}: did not build: $(awk '/"severity":"error"/' "${WORK}/${name}.b" | head -1 | cut -c1-240)"; continue
        fi
        local got; got="$("${WORK}/${name}" </dev/null 2>&1 | tr '\n' '|')"
        if [ "${got}" = "${want}" ]; then pass "${name} ${o}: ${want}"
        else fail "${name} ${o}: got [${got}] want [${want}]"; fi
    done
}

# 1. references through a local: i64, f64, str, and a re-bind that shadows.
case_ fref '3|2.5|hi bob|7|' '  let g=&add;
  io.println("\(g(1;2))");
  let h=&half;
  io.println("\(h(5.0))");
  let k=&hi;
  io.println(k("bob"));
  let g=&sub;
  io.println("\(g(9;2))");'
# 2. an f64 capture: 2.0 * 2.5 = 5.
case_ capf64 '5|' '  let k=2.5;
  let sk=fn(x:f64):f64{<x*k};
  io.println("\(sk(2.0))");'
# 3. an @f64 parameter and an f64 result: element 1 of (1.5;2.5).
case_ arrf64 '2.5|' '  let ff=fn(a:@f64):f64{<a.get(1)};
  io.println("\(ff(@(1.5;2.5)))");'
# 4. an @f64 capture read inside the closure, times an f64 parameter: 1.5*2.
case_ capaf64 '3|' '  let ww=@(1.5;4.0);
  let hh=fn(k:f64):f64{<ww.get(0)*k};
  io.println("\(hh(2.0))");'
# 5. a closure applied on the spot: 7+5, and its env is released.
case_ direct '12|' '  let k=5;
  io.println("\(fn(x:i64):i64{<x+k}(7))");'
# 6. a closure-bound local in one f= must not capture a same-named local in
#    another: `g` is a closure in one(), a function reference in main().
#    (The binding table was module-wide and first-match: invalid IR.)
#    one() = 1+1 = 2; g(1;2) = 3; and a closure calling a captured closure,
#    (3+1)*2 = 8, still resolves inside the lifted body.
cat > "${WORK}/scope.tk" <<'EOF'
m=c128s;
i=io:std.io;
f=add(a:i64;b:i64):i64{<a+b};
f=one():i64{
  let g=fn(x:i64):i64{<x+1};
  <g(1)
};
f=main():i64{
  let g=&add;
  io.println("\(one()) \(g(1;2))");
  let f=fn(x:i64):i64{<x+1};
  let h=fn(y:i64):i64{<f(y)*2};
  io.println("\(h(3))");
  <0
};
EOF
for o in -O0 -O2; do
    if "${TKC}" --allow-all "${o}" --out "${WORK}/scope" "${WORK}/scope.tk" >"${WORK}/scope.b" 2>&1; then
        got="$("${WORK}/scope" </dev/null 2>&1 | tr '\n' '|')"
        if [ "${got}" = "2 3|8|" ]; then pass "scope ${o}: ${got}"
        else fail "scope ${o}: got [${got}] want [2 3|8|]"; fi
    else
        fail "scope ${o}: did not build: $(awk '/"severity":"error"/' "${WORK}/scope.b" | head -1 | cut -c1-240)"
    fi
done
if "${TKC}" --allow-all --emit-llvm --out "${WORK}/direct.ll" "${WORK}/direct.tk" >/dev/null 2>&1 &&
   awk '/call void @free\(i8\* %t[0-9]+\) ; 124.0c env released/{f=1} END{exit !f}' "${WORK}/direct.ll"; then
    pass "direct: the applied closure's env is freed"
else
    fail "direct: no free of the env box in the emitted IR"
fi

echo ""
echo "C128: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
