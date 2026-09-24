#!/usr/bin/env bash
# C050_return_mismatch_fix_certain.sh — a return-type E4031 carries a `fix`
# only when that fix is certain (AGENTS.md §6), and the ooke typed-empty
# return shape checks clean.
#
# THE DEFECT. Every return-type mismatch carried
# `"fix":"cast return value to T using 'as'"`. toke has no `as` cast between
# arrays, strings and numbers, so for most mismatches that instruction does
# not compile. It surfaced on ooke's
#
#     f=mk():@($pt){ let ps=mut.@($pt); ps=ps.append($pt{x:1}); <ps };
#
# which 886bdb0 rejected with "expected '@(pt)', got '@(void)'" (the typed-
# empty regression, fixed in e2e4ac4) and told the repair loop to cast an
# array. The fix now appears only for a cast that compiles and cannot change
# the value: any integer into i64, f32 into f64.
#
# WHAT IS ASSERTED:
#   1. The ooke shape checks clean and runs.
#   2. Non-scalar, int<->float and str mismatches: E4031 with an EMPTY fix.
#   3. A widening integer mismatch keeps the fix, and APPLYING the fix
#      produces a program that checks clean and returns the value unchanged.
#
# Negative control: TKC built from 444a55c fails 1 (E4031 '@(void)') and 2
# (fix populated).
#
# Story: fix to 886bdb0 (127.129) regression; §6

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"E[0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
fixof() { "${TKC}" --check "$1" 2>&1 | awk '/"error_code":"E4031"/ && match($0,/"fix":"[^"]*"/){print substr($0,RSTART+7,RLENGTH-8); exit}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/ooke.tk" <<'EOF'
m=t;
t=$pt{x:i64};
f=mk():@($pt){ let ps=mut.@($pt); ps=ps.append($pt{x:1}); <ps };
f=main():i64{ let n=mk().len; <n as i64 };
EOF
expect "1-ooke-typed-empty-return-checks" "$(codes "${WORK}/ooke.tk")" ""
"${TKC}" "${WORK}/ooke.tk" -o "${WORK}/ooke" >/dev/null 2>&1; "${WORK}/ooke" 2>/dev/null
expect "1-ooke-typed-empty-return-runs" "$?" "1"

n=0
while IFS= read -r fn; do
    [ -z "$fn" ] && continue; n=$((n + 1))
    printf 'm=t;\n%s\nf=main():i64{ <0 };\n' "$fn" > "${WORK}/m$n.tk"
    expect "2.$n-E4031-no-fix: $fn" "$(codes "${WORK}/m$n.tk")|$(fixof "${WORK}/m$n.tk")" "E4031 |"
done <<'EOF'
f=g():@(i64){ <"x" };
f=g():i64{ <1.5 };
f=g():str{ <1 };
f=g():@(i64){ <@("a") };
EOF

printf 'm=t;\nf=g():i64{ let x:i32=7 as i32; <x };\nf=main():i64{ <g() };\n' > "${WORK}/w.tk"
expect "3-widening-keeps-fix" "$(fixof "${WORK}/w.tk")" "cast return value to i64 using 'as'"
sed 's/<x }/<x as i64 }/' "${WORK}/w.tk" > "${WORK}/w2.tk"
expect "3-fix-injection-landed" "$(grep -c 'x as i64' "${WORK}/w2.tk")" "1"
expect "3-applied-fix-checks" "$(codes "${WORK}/w2.tk")" ""
"${TKC}" "${WORK}/w2.tk" -o "${WORK}/w2" >/dev/null 2>&1; "${WORK}/w2" 2>/dev/null
expect "3-applied-fix-keeps-value" "$?" "7"

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
