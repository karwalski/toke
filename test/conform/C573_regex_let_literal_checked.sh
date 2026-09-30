#!/usr/bin/env bash
# C573_regex_let_literal_checked.sh — E4075 also checks a regex pattern held
# by an immutable `let` bound to a literal, when that binding is provably the
# one the call reads (121.45 (3)).
#
# THE DEFECT.  regex_check.c saw only `alias.fn(subject; "literal")`.
# `let p="a(b"; str.containsre(x;p)` compiled and stopped at run time with
# RT008.  The check now follows an identifier pattern to its `let`, when, in
# the enclosing function, the name has exactly one binder (that `let`), every
# other occurrence is a read, and the call sits after the `let` in its
# statement list.  Anything else (a mut binding, a shadowing `let`, a
# parameter, a match-arm binding) is left to the run-time check.
#
# Cases:
#   1 let p="a(b" read by containsre and countre -> exactly ONE E4075, at the
#     `(` inside the let's literal (line 3, col 11); no fix field
#   2 a valid let pattern still builds and runs; a parameter named p in
#     another function is not confused with main's let: "1 1"
#   3 a shadowing inner `let p="b"` over an invalid outer p, and a mut
#     binding reassigned to a valid pattern: no E4075, builds, runs "1 1"
#   4 the literal form is unchanged (E4075 at the literal, C434 covers more)
#
# Negative control: TKC=<toke built at main cde89021> — case 1 red (0 E4075:
# the program compiles); 2, 3 and 4 green.
#
# Story: 121.45 (3)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# build NAME -> "E4075-count line:col fix-present" or "built|<output>"
build() {
    if "${TKC}" --allow-all "${WORK}/$1.tk" -o "${WORK}/$1" > "${WORK}/$1.log" 2>&1; then
        echo "built|$(cd "${WORK}" && "./$1" 2>&1 | tr '\n' ' ' | sed 's/ $//')"
    else
        local n; n="$(grep -o '"error_code":"E4075"' "${WORK}/$1.log" | wc -l | tr -d ' ')"
        local at; at="$(grep '"E4075"' "${WORK}/$1.log" | head -1 | sed -n 's/.*"line":\([0-9]*\),"col":\([0-9]*\).*/\1:\2/p')"
        local fx; fx="$(grep '"E4075"' "${WORK}/$1.log" | grep -c '"fix"')"
        echo "${n} ${at} fix=${fx}"
    fi
}
cat > "${WORK}/c1.tk" <<'EOF'
m=main;i=io:std.io;i=s:std.str;
f=main():i64{
  let p="a(b";
  io.println(s.fromint(s.containsre("xab";p)));
  io.println(s.fromint(s.countre("xab";p)));
  <0
};
EOF
cat > "${WORK}/c2.tk" <<'EOF'
m=main;i=io:std.io;i=s:std.str;
f=g(p:str):i64{<s.containsre("abc";p)};
f=main():i64{
  let p="b+";
  io.println(s.fromint(s.containsre("abc";p)));
  io.println(s.fromint(g("c")));
  <0
};
EOF
cat > "${WORK}/c3.tk" <<'EOF'
m=main;i=io:std.io;i=s:std.str;
f=main():i64{
  let p="(";
  if(1>0){let p="b"; io.println(s.fromint(s.containsre("abc";p)))};
  let q=mut."(";
  q="c";
  io.println(s.fromint(s.containsre("abc";q)));
  <0
};
EOF
cat > "${WORK}/c4.tk" <<'EOF'
m=main;i=io:std.io;i=s:std.str;
f=main():i64{
  io.println(s.fromint(s.containsre("xab";"a(b")));
  <0
};
EOF
expect "1-let-literal-one-E4075-located" "$(build c1)" "1 3:11 fix=0"
expect "2-valid-let-and-param"           "$(build c2)" "built|1 1"
expect "3-shadow-and-mut-left-to-runtime" "$(build c3)" "built|1 1"
expect "4-inline-literal-unchanged"      "$(build c4)" "1 3:45 fix=0"
echo "C573: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
