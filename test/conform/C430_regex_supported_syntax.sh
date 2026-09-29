#!/usr/bin/env bash
# C430_regex_supported_syntax.sh — the regex language means the same thing on
# every platform (story 137.9).
#
# THE DEFECT.  str.* regex calls ran POSIX regcomp/regexec.  On macOS \s \d \w
# \b compiled as the LITERAL letters s d w b (glibc read them as GNU classes),
# and (?i) failed to compile and read as "no match".  So `str.replacere("a
# b";"\\s";"_")` returned "a b" on macOS -- loke T-9, whose script-stripping
# privacy sanitiser matched nothing for months.
#
# WHAT IS ASSERTED (one program; computed patterns, so no compile-time check
# is involved -- this is the runtime engine, src/stdlib/tk_regex.h):
#   \s \S \d \w \b, a leading (?i), [:classes:], {n,m}, POSIX
#   leftmost-longest alternation, the negated-class idiom for <.*?>.
#
# Negative control: C430_STDLIB=<main's src/stdlib> (the regcomp glue) --
# measured on Darwin 25.6: the \s \S \d \w \b and (?i) lines differ.
#
# Story: 137.9
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C430_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c430_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cat > "${WORK}/p.tk" <<'TOKE'
m=main;
i=io:std.io;
i=s:std.str;
f=p(x:$str):$str{<s.concat(x;"")};
f=main():i64{
  io.println(s.replacere("a b\tc";p("\\s");"_"));
  io.println(s.replacere("a b";p("\\S");"_"));
  io.println(s.fromint(s.countre("a1b22c333";p("\\d+"))));
  io.println(s.replacere("--ab_9--";p("\\w+");"W"));
  io.println(s.replacere("foo afoo foo";p("\\bfoo\\b");"X"));
  io.println(s.replacere("xABCx";p("(?i)abc");"Z"));
  io.println(s.replacere("12ab3";p("[[:alpha:]]+");"L"));
  io.println(s.replacere("aaaaa";p("a{2,3}");"."));
  io.println(s.replacere("ab";p("a|ab");"X"));
  io.println(s.replacere("<x>y<z>";p("<[^>]*>");"[t]"));
  <0
};
TOKE
if ! "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; sed 's/^/        /' "${WORK}/build.log" | head -5; exit 1
fi
out="$("${WORK}/b" 2>&1)"; rc=$?
line() { printf '%s\n' "$out" | sed -n "$1p"; }
expect "0-exit"        "$rc" "0"
expect "1-\\s"         "$(line 1)" "a_b_c"
expect "2-\\S"         "$(line 2)" "_ _"
expect "3-\\d+ count"  "$(line 3)" "3"
expect "4-\\w+"        "$(line 4)" "--W--"
expect "5-\\b"         "$(line 5)" "X afoo X"
expect "6-(?i)"        "$(line 6)" "xZx"
expect "7-[:alpha:]"   "$(line 7)" "12L3"
expect "8-{2,3}"       "$(line 8)" ".."
expect "9-longest"     "$(line 9)" "X"
expect "10-negclass"   "$(line 10)" "[t]y[t]"
echo "C430: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
