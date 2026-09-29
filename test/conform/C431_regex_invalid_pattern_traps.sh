#!/usr/bin/env bash
# C431_regex_invalid_pattern_traps.sh — an invalid pattern computed at run
# time is an error, never "no match" (story 137.9).
#
# THE DEFECT.  `if (regcomp(...) != 0) return 0;` -- every regex entry point
# returned its no-match value (0, 0, [], 0, the input unchanged) when the
# pattern did not compile, so `*?`, `(?i)` in the middle, `\1` or a malformed
# `[` ran and quietly did nothing.  A literal pattern is now E4075 at compile
# time (D140-D143); this covers patterns the compiler cannot see.
#
# WHAT IS ASSERTED, for each of str.containsre, str.countre, str.findall,
# str.matches and str.replacere with a computed invalid pattern: exit 1,
# stderr carries `RT008: invalid regex pattern in str.<fn>` with the byte
# offset, and the line after the call is never printed.
#
# Negative control: C431_STDLIB=<main's src/stdlib> -- all five exit 0 and
# print the no-match value (measured on Darwin 25.6).
#
# Story: 137.9
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C431_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c431_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
# run NAME CALL : CALL uses `p` (the computed pattern); prints "rc|RT008 line"
run() {
    cat > "${WORK}/$1.tk" <<TOKE
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let p:str=s.concat("$2";"");
  let r=$3;
  io.println("after the call");
  <0
};
TOKE
    rm -f "${WORK}/$1"
    "${TKC}" --allow-all "${WORK}/$1.tk" -o "${WORK}/$1" > "${WORK}/$1.log" 2>&1 || { echo "build-failed"; return; }
    "${WORK}/$1" > "${WORK}/$1.out" 2> "${WORK}/$1.err"; local rc=$?
    local t; t="$(grep -o 'RT008: invalid regex pattern in str\.[a-z]*: .* (at byte [0-9]*' "${WORK}/$1.err" | sed -E 's/(str\.[a-z]+): .* \(at byte/\1: … (at byte/')"
    local a=""; grep -q 'after the call' "${WORK}/$1.out" && a=" +after"
    echo "${rc}|${t}${a}"
}
expect "1-containsre lazy"   "$(run c1 '<.*?>'  's.containsre("<a>";p)')"      '1|RT008: invalid regex pattern in str.containsre: … (at byte 3'
expect "2-countre backref"   "$(run c2 '(a)\\1' 's.countre("aa";p)')"        '1|RT008: invalid regex pattern in str.countre: … (at byte 3'
expect "3-findall bracket"   "$(run c3 'a[b'    's.findall("ab";p)')"          '1|RT008: invalid regex pattern in str.findall: … (at byte 1'
expect "4-matches unbalanced" "$(run c4 'a)(b'  's.matches("ab";p)')"          '1|RT008: invalid regex pattern in str.matches: … (at byte 1'
expect "5-replacere mid (?i)" "$(run c5 'x(?i)a' 's.replacere("xa";p;"_")')"   '1|RT008: invalid regex pattern in str.replacere: … (at byte 1'
echo "C431: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
