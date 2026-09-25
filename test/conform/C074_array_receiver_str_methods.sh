#!/usr/bin/env bash
# C074_array_receiver_str_methods.sh — a str-only receiver method on an array
# literal, and `.join` on an array whose elements are not strings, are E4036
# at --check instead of a build that segfaults (story 127.168).
#
# THE DEFECT. Measured at main 2e932d6:
#
#     @(1;2;3).replace(1;2)     --check 0, build 0, run rc=139
#     @(1;2).join(",")          --check 0, build 0, run rc=139
#     let xs=@(1;2); xs.join(",")   same
#     ",".join(xs)  (xs @i64)   same
#
# Two holes. 127.151's receiver-kind check judged only identifier and
# string-literal receivers, so an array LITERAL was never judged. And `.join`
# is a real array method, but both of its lowerings (tk_arr_join_w,
# tk_str_join_w) read every element as a char*, so an @i64's first element
# was dereferenced as an address.
#
# WHY THIS CAN'T FIRE ON A CORRECT PROGRAM. A literal's kind is its shape,
# which the emitter reads the same way (expr_struct_type "@…"); the join
# check fires only when the element type is KNOWN and is not str.
#
# WHAT IS ASSERTED:
#   1. The four shapes above are E4036 with an empty `fix`.
#   2. The right-kind controls on literals check clean and run to the right
#      answer: @str.join on a literal, a variable and a separator receiver;
#      array-literal contains/len/indexof; a map-literal getor.
#
# Negative control: TKC=<main 2e932d6 build> gives exit 0 at --check for
# every case in 1 (5 FAIL).
#
# Story: 127.168

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "SKIP: tkc binary not found at ${TKC}"; exit 1; fi
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
prog() { printf 'm=t;\ni=io:std.io;\nf=main():i64{\n%s\n  <0\n};\n' "$2" > "${WORK}/$1.tk"; }

echo "C074: str-only methods on an array receiver, .join on non-str elements"
echo "----------------------------------------------------------------------"

# ── 1. rejected ──────────────────────────────────────────────────────────
prog lit_replace '  let a=@(1;2;3).replace(1;2);
  io.println("\(a)");'
expect "1-array-literal-replace-is-E4036" "$(codes "${WORK}/lit_replace.tk")" "E4036 "
prog lit_upper '  io.println(@(1;2).upper());'
expect "1-array-literal-upper-is-E4036" "$(codes "${WORK}/lit_upper.tk")" "E4036 "
prog lit_join '  io.println(@(1;2).join(","));'
expect "1-int-array-literal-join-is-E4036" "$(codes "${WORK}/lit_join.tk")" "E4036 "
prog var_join '  let xs=@(1;2);
  io.println(xs.join(","));'
expect "1-int-array-var-join-is-E4036" "$(codes "${WORK}/var_join.tk")" "E4036 "
prog sep_join '  let xs=@(1;2);
  io.println(",".join(xs));'
expect "1-sep-join-int-array-is-E4036" "$(codes "${WORK}/sep_join.tk")" "E4036 "
expect "1-fix-field-empty" \
    "$("${TKC}" --check "${WORK}/lit_join.tk" 2>&1 | grep -c '"fix":""' || true)" "1"

# ── 2. right kind: checks clean and runs ─────────────────────────────────
cat > "${WORK}/good.tk" <<'EOF'
m=t;
i=io:std.io;
f=main():i64{
  let w=@("x";"y");
  io.println(@("a";"b";"c").join("+"));
  io.println("\(w.join("-")) \(",".join(w))");
  io.println("\(@(1;2;3).len) \(@(5;6;7).indexof(6))");
  if(@(1;2;3).contains(2)){io.println("contains")};
  io.println(@("k":"v").getor("k";"z"));
  <0
};
EOF
expect "2-right-kind-checks" "$(codes "${WORK}/good.tk")" ""
"${TKC}" --allow-all --out "${WORK}/good" "${WORK}/good.tk" >/dev/null 2>&1
expect "2-right-kind-runs" "$("${WORK}/good" 2>/dev/null | tr '\n' '|')" \
    "a+b+c|x-y x,y|3 1|contains|v|"

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
