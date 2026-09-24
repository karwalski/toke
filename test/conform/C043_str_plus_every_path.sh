#!/usr/bin/env bash
# C043_str_plus_every_path.sh — `+` on two strings is E4031 no matter where
# the strings come from (story 127.157).
#
# THE RULE. `+` is numeric only: toke-spec-v0.3 operator table ("`+` add
# (numeric only)"), not superseded by the v0.4 amendment; ADR-0004; the
# breaking-v0.3.x-string-concat decision ("This applies to every form").
#
# THE DEFECT. The rule fired when the operands were parameters or literals,
# and not when they were un-annotated locals: `let a="x"; let b="y"; a+b`
# passed --check and printed "xy", because bind_init_type() had no NODE_STR_LIT
# case, so `a` and `b` were TY_UNKNOWN and the rule cannot fire on an unknown.
# The local-binding path was the false negative; the parameter path was right.
#
# The same missing case left `let s="hello"; s.get(0)` unchecked (127.128's
# open half): the index rule could not see that `s` is a str, and the program
# compiled and read the string's bytes as an array header at run time.
#
# WHAT IS ASSERTED:
#   1. Every shape gives the SAME diagnostic — code E4031 and the same
#      headline: parameters, literals, two locals, a `mut` local, a local and
#      a literal. "Both paths agree" is the acceptance criterion, so it is
#      asserted as equality of the headline, not merely of the code.
#   2. The E4031 carries the ADR-0004 `fix` on the local path too.
#   3. Reported once, not once per reference: infer() re-enters a binding's
#      initialiser on every use.
#   4. Typing string locals did not break correct string code: s.concat,
#      interpolation, .len and a chained binding still compile and run.
#   5. 127.128: `.get(0)` on a let-bound string is E4031 at --check.
#
# Negative control: run with TKC pointing at a compiler built from e2e4ac4
# (before this change); 1c/1d/1e, 2 and 5 go red.
#
# Story: 127.157 (and the open half of 127.128)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
headline() { "${TKC}" --check "$1" 2>&1 | awk 'match($0,/"message":"[^"]*"/){print substr($0,RSTART+11,RLENGTH-12); exit}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

prog() {  # prog NAME BODY  — a main() with io imported
    printf 'm=t;\ni=io:std.io;\nf=main():i64{\n%s\n  <0\n};\n' "$2" > "${WORK}/$1.tk"
}
cat > "${WORK}/param.tk" <<'EOF'
m=t;
i=io:std.io;
f=cat(a:str;b:str):str{
  <a+b
};
f=main():i64{
  io.println(cat("x";"y"));
  <0
};
EOF
prog lit      '  io.println("x"+"y");'
prog locals   '  let a="x";
  let b="y";
  io.println(a+b);'
prog mutlocal '  let a=mut."x";
  let b="y";
  a=a+b;
  io.println(a);'
prog mixed    '  let a="x";
  let c=a+"y";
  io.println(c);'
prog reused   '  let a="x";
  let b=a;
  io.println(a+b);
  io.println(b);
  io.println(a);'
cat > "${WORK}/good.tk" <<'EOF'
m=t;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let a="x";
  let b=a;
  let n=a.len;
  io.println(s.concat(a;"y"));
  io.println("\(a)\(b) \(n)");
  <0
};
EOF
prog strget   '  let s="hello";
  let c=s.get(0);
  io.println("\(c)");'

WANT="'+' requires numeric operands: both sides are 'str', and toke does not overload '+' for string concatenation"

expect "1a-parameters"      "$(codes "${WORK}/param.tk")|$(headline "${WORK}/param.tk")" "E4031 |${WANT}"
expect "1b-literals"        "$(codes "${WORK}/lit.tk")|$(headline "${WORK}/lit.tk")" "E4031 |${WANT}"
expect "1c-two-locals"      "$(codes "${WORK}/locals.tk")|$(headline "${WORK}/locals.tk")" "E4031 |${WANT}"
expect "1d-mut-local"       "$(codes "${WORK}/mutlocal.tk")|$(headline "${WORK}/mutlocal.tk")" "E4031 |${WANT}"
expect "1e-local-plus-lit"  "$(codes "${WORK}/mixed.tk")|$(headline "${WORK}/mixed.tk")" "E4031 |${WANT}"

expect "2-local-path-carries-adr0004-fix" \
    "$("${TKC}" --check "${WORK}/locals.tk" 2>&1 | grep -c '"fix":"`+` is numeric-only in toke' || true)" "1"

expect "3-reported-once" "$(codes "${WORK}/reused.tk")" "E4031 "

expect "4-correct-string-code-checks" "$(codes "${WORK}/good.tk")" ""
"${TKC}" "${WORK}/good.tk" -o "${WORK}/good" >/dev/null 2>&1
expect "4-correct-string-code-runs" "$("${WORK}/good" 2>/dev/null | tr '\n' '|')" "xy|xx 1|"

expect "5-get-on-let-bound-str-is-E4031" "$(codes "${WORK}/strget.tk")" "E4031 "

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
