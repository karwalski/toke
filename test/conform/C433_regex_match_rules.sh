#!/usr/bin/env bash
# C433_regex_match_rules.sh — str.matches is whole-string by rule, and the
# find-next functions scan from the right places (story 137.9).
#
# THE DEFECTS.
#   * str.matches pasted "^(" and ")$" around the pattern text -- undocumented,
#     and it turned the unbalanced `a)(b` into the valid `^(a)(b)$`.
#   * countre/findall/replacere called regexec on the remaining suffix, so `^`
#     matched at every restart point: countre("aaa";"^a") was 3.
#   * after an empty match at the end they advanced past the terminator
#     (`p++; if (!*p)` -- a heap read one byte out, seen under ASan), and never
#     tried the end position, so replacere("abc";"x*";"-") was "-a-b-c".
#
# WHAT IS ASSERTED (computed patterns): the whole-string rule, a computed
# `a)(b` in str.matches is RT008, `^` anchors to the string start only, and
# the empty-match rule (after an empty match move one byte on; an empty
# match directly after the previous match is skipped).
#
# Negative control: C433_STDLIB=<main's src/stdlib> -- 3, 4, 5, 6 and 9 fail; 6 read 3 for countre("";"x*"), the count
# of whatever lay past the terminator
# (measured on Darwin 25.6).
#
# Story: 137.9
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C433_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c433_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'TOKE'
m=main;
i=io:std.io;
i=s:std.str;
f=p(x:$str):$str{<s.concat(x;"")};
f=main():i64{
  io.println(s.fromint(s.matches("123";p("[0-9]+"))));
  io.println(s.fromint(s.matches("x123";p("[0-9]+"))));
  io.println(s.fromint(s.countre("aaa";p("^a"))));
  io.println(s.replacere("abc";p("x*");"-"));
  io.println(s.replacere("abxd";p("x*");"-"));
  io.println(s.fromint(s.countre("";p("x*"))));
  let f=s.findall("ab";p("a|ab"));
  io.println(s.fromint(f.len));
  io.println(s.fromint(s.countre("a1b2";p("[0-9]$"))));
  <0
};
TOKE
"${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1 || { echo "FAIL build"; head -5 "${WORK}/build.log"; exit 1; }
out="$("${WORK}/b" 2>&1)"
line() { printf '%s\n' "$out" | sed -n "$1p"; }
expect "1-matches whole"        "$(line 1)" "1"
expect "2-matches not search"   "$(line 2)" "0"
expect "3-^ at start only"      "$(line 3)" "1"
expect "4-empty matches abc"    "$(line 4)" "-a-b-c-"
expect "5-empty after match"    "$(line 5)" "-a-b-d-"
expect "6-empty subject"        "$(line 6)" "1"
expect "7-findall longest"      "$(line 7)" "1"
expect "8-\$ at end only"       "$(line 8)" "1"
cat > "${WORK}/u.tk" <<'TOKE'
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let q:str=s.concat("a)(b";"");
  io.println(s.fromint(s.matches("ab";q)));
  <0
};
TOKE
"${TKC}" --allow-all "${WORK}/u.tk" -o "${WORK}/u" > "${WORK}/u.log" 2>&1 || { echo "FAIL build u"; exit 1; }
"${WORK}/u" > "${WORK}/u.out" 2> "${WORK}/u.err"; rc=$?
expect "9-matches a)(b is RT008" "${rc}|$(grep -o '^RT008: invalid regex pattern in str.matches' "${WORK}/u.err")|$(cat "${WORK}/u.out")" \
       "1|RT008: invalid regex pattern in str.matches|"
echo "C433: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
