#!/usr/bin/env bash
# C800_libm_name_substring_import.sh — an imported user function whose name
# CONTAINS a libm name does not stop tkc declaring that libm function
# (127.305).
#
# The stdlib-declaration loop in emit_llvm_ir skipped a declaration when the
# body already declared the symbol.  Its test accepted any occurrence of the
# bare name on a `declare` line, so `declare i1 @util_isink(double)` (from an
# imported util.isink) counted as a declaration of `sin`, `declare double
# @sin(double)` was never emitted, and clang rejected the module with
# "use of undefined value '@sin'" (E9003).  Every libm name std.math maps to
# was exposed the same way.
#
# For EVERY libm name in the std.math table (sin cos tan asin acos atan atan2
# fmod fabs log log10 exp round sqrt floor ceil pow) this generates the
# three-module shape of the original report:
#     util.q<name>k   — exported predicate whose name contains <name>
#     lib.s(x)        — imports util and std.math, calls util.q<name>k and
#                       math.<name>
#     main            — prints lib.s(0.5)
# builds it with `tkc -o`, and requires it to build, run, and print exactly
# what the control (util's export renamed to a name containing no libm name)
# prints.
#
# Negative control (recorded in 127.305): tkc at main 8bb9f63e fails the build
# for every one of the 17 names with E9003 "use of undefined value '@<name>'".
#
# Story: 127.305

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc not found at ${TKC}"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/c800.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0
FAIL=0
ok()  { echo "PASS: $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL: $1"; FAIL=$((FAIL + 1)); }

# gen <dir> <util-fn-name> <libm-name> <arity>
gen() {
    local d="$1" un="$2" lm="$3" ar="$4" call
    mkdir -p "$d"
    if [ "$ar" = 2 ]; then call="math.${lm}(x;0.25)"; else call="math.${lm}(x)"; fi
    cat > "$d/util.tk" <<EOF
m=util;
f=${un}(v:f64):bool{<v>0.0};
EOF
    cat > "$d/lib.tk" <<EOF
m=lib;
i=math:std.math;
i=util:util;
f=s(x:f64):f64{<if(util.${un}(x)){${call}}el{0.0}};
EOF
    cat > "$d/main.tk" <<'EOF'
m=main;
i=io:std.io;
i=lib:lib;
f=main():i64{
  io.println("\(lib.s(0.5) as $str)");
  <0
};
EOF
}

# build+run; prints output on stdout, returns non-zero on failure
build_run() {
    local d="$1"
    ( cd "$d" && "${TKC}" -o p util.tk lib.tk main.tk > build.log 2>&1 ) || return 1
    "$d/p" 2>&1
}

for spec in sin:1 cos:1 tan:1 asin:1 acos:1 atan:1 atan2:2 fmod:2 fabs:1 \
            log:1 log10:1 exp:1 round:1 sqrt:1 floor:1 ceil:1 pow:2; do
    lm="${spec%%:*}"; ar="${spec##*:}"
    gen "${WORK}/${lm}" "q${lm}k" "${lm}" "${ar}"
    gen "${WORK}/${lm}_ctl" "qzk" "${lm}" "${ar}"
    if ! ctl="$(build_run "${WORK}/${lm}_ctl")"; then
        bad "${lm}: control (no libm substring) failed to build/run: $(head -c 300 "${WORK}/${lm}_ctl/build.log")"
        continue
    fi
    if ! got="$(build_run "${WORK}/${lm}")"; then
        bad "${lm}: util.q${lm}k + math.${lm} failed to build/run: $(head -c 300 "${WORK}/${lm}/build.log")"
        continue
    fi
    if [ "$got" = "$ctl" ] && [ -n "$got" ]; then
        ok "${lm}: util.q${lm}k + math.${lm} builds and prints ${got} (= control)"
    else
        bad "${lm}: printed '${got}', control printed '${ctl}'"
    fi
done

# The original report's spellings: isink, and an export named sinks.
for un in isink sinks; do
    gen "${WORK}/orig_${un}" "${un}" sin 1
    if got="$(build_run "${WORK}/orig_${un}")" && [ "$got" = "$(build_run "${WORK}/sin_ctl")" ]; then
        ok "original spelling util.${un}: builds and prints ${got}"
    else
        bad "original spelling util.${un}: $(head -c 300 "${WORK}/orig_${un}/build.log")"
    fi
done

echo "C800: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
