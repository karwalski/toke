#!/usr/bin/env bash
# C432_regex_work_is_bounded.sh — no regex call can run away (story 121.35).
#
# THE DEFECT.  POSIX regcomp/regexec with no budget.  Measured on Darwin 25.6
# (Apple M2 Pro): the find-next loops of countre/findall/replacere re-scanned
# the tail after every match, so `a|a*b` over 16 000 bytes took 2.4 s and x4
# per doubling; `(a{1,255}){1,255}` took 1.2 GB inside regcomp.
#
# THE FIX.  src/stdlib/tk_regex.h: a Thompson-NFA matcher (one search is
# linear, no backtracking), a 2000-state cap after repeat expansion, and a
# per-call step budget -- 8 full passes over the subject + 2^26 steps.
#
# WHAT IS ASSERTED (patterns computed at run time, so the compile-time check
# is not involved):
#   1. countre("a|a*b") over 16 000 bytes stops with RT009, exit 1, in < 10 s
#   2. the same over 160 000 bytes also stops in < 10 s (bounded, not O(n^2))
#   3. containsre("(a+)+b") over 1 000 000 bytes -- the textbook backtracking
#      blow-up -- answers 0 in < 10 s
#   4. countre("\\w+") over 200 000 words finds all 200 000: a legitimate
#      scan never meets the budget
#   5. "(a{1,255}){1,255}" is RT008 "pattern too large", not an allocation
#
# Negative control: C432_STDLIB=<main's src/stdlib> -- 1, 2 and 5 fail (1 and
# 2 run to a count with no RT009; 2 would take ~6 minutes, extrapolated from 3.6 s at 16 000, so the control skips it
# unless C432_SLOW=1); 3 and 4 pass on regexec too.
#
# Story: 121.35
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${C432_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c432_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
# run NAME PATTERN SUBJECT-EXPR CALL -> "rc|<first RTnnn code>|stdout|fast|slow"
run() {
    cat > "${WORK}/$1.tk" <<TOKE
m=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let p:str=s.concat("$2";"");
  let x:str=$3;
  io.println(s.fromint($4));
  <0
};
TOKE
    "${TKC}" --allow-all "${WORK}/$1.tk" -o "${WORK}/$1" > "${WORK}/$1.log" 2>&1 || { echo "build-failed"; return; }
    local t0 t1 rc
    t0=$(python3 -c 'import time;print(time.time())')
    "${WORK}/$1" > "${WORK}/$1.out" 2> "${WORK}/$1.err"; rc=$?
    t1=$(python3 -c 'import time;print(time.time())')
    local speed; speed=$(python3 -c "print('fast' if $t1-$t0 < 10 else 'slow(%.1fs)' % ($t1-$t0))")
    local code; code="$(grep -o '^RT00[0-9]' "${WORK}/$1.err" | head -1)"
    local msg=""; grep -q 'pattern too large' "${WORK}/$1.err" && msg=" too-large"
    echo "${rc}|${code}${msg}|$(tr -d '\n' < "${WORK}/$1.out")|${speed}"
}
expect "1-quadratic loop 16k"  "$(run q16 'a|a*b' 's.repeat("a";16000)' 's.countre(x;p)')"  "1|RT009||fast"
if [ -z "${C432_STDLIB:-}" ] || [ -n "${C432_SLOW:-}" ]; then
expect "2-quadratic loop 160k" "$(run q160 'a|a*b' 's.repeat("a";160000)' 's.countre(x;p)')" "1|RT009||fast"
else echo "SKIP 2-quadratic loop 160k (control run: ~6 min on regexec, extrapolated; C432_SLOW=1 to run it)"; fi
expect "3-(a+)+b 1M"           "$(run bt '(a+)+b' 's.repeat("a";1000000)' 's.containsre(x;p)')" "0||0|fast"
expect "4-legit \\w+ scan"     "$(run w '\\w+' 's.repeat("word ";200000)' 's.countre(x;p)')"     "0||200000|fast"
expect "5-expansion cap"       "$(run big '(a{1,255}){1,255}' '"aaaa"' 's.countre(x;p)')"         "1|RT008 too-large||fast"
echo "C432: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
