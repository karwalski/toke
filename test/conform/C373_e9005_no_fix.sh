#!/usr/bin/env bash
# C373_e9005_no_fix.sh — E9005 carries no `fix` field (127.237 (4), Batch
# GB2; AGENTS.md §6).
#
# THE DEFECT.  E9005 ("match has N arms, but the sum type of the matched
# value is not established") carried
#   "fix": "annotate the matched value with its sum type, or match it in a
#           function that takes it as a typed parameter; …"
# — two alternatives, neither an edit, and wrong outright when the arms are
# not variants of any sum at all.  The `fix` field is only populated when
# the fix is deterministically correct in every case the error is emitted;
# it is now absent.  The message is unchanged.
#
# WHY A C-SUITE AND NOT A D-SERIES YAML.  E9005 is a codegen diagnostic, and
# test/run_conform.sh runs every D-series input under `--check`, which stops
# before codegen: a D yaml sees exit 0 and no E9005 at all (measured).  So
# the pin is here, on a real build, asserting the record exactly.
#
# Negative control: TKC built from main at 9f901bf — cases 1 and 2 red (the
# record carries "fix"); each case also asserts E9005 is still emitted once
# with its message unchanged.
#
# Story: 127.237 (4)

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
# e9005 SRC: "<records with E9005> <of those, records with a fix key> <message>"
e9005() {
    printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --emit-llvm "${WORK}/p.tk" -o "${WORK}/p.ll" 2>&1 >/dev/null | python3 -c '
import sys, json
recs = []
for line in sys.stdin:
    line = line.strip()
    if not line.startswith("{"): continue
    try: d = json.loads(line)
    except Exception: continue
    if d.get("error_code") == "E9005": recs.append(d)
print(len(recs), sum(1 for d in recs if "fix" in d), recs[0]["message"] if recs else "-")'
}

# 1. The sum type is not established (a sum passed through an i64 param).
#    138.37: over the i64 parameter itself the checker now refuses the arms
#    first (E4031); a `mut` copy is not judged at check, so codegen's E9005
#    is still what stops it.
expect "1-unestablished" "$(e9005 'm=main;
f=pick(sh:i64):i64{
  let s=mut.sh;
  <mt(s){$circle:x 10;$square:x 20;$tri:x 30}
};
f=main():i64{<pick(0)};')" "1 0 match has 3 arms, but the sum type of the matched value is not established"

# 2. The other message form: a type resolved, but the first arm names none
#    of its variants (a std call's `$str` result, first arm `$ok`, so the
#    string chain declines — see C370).  127.242 (3): a str is not a sum
#    type, and the message no longer calls it one ("... is not a variant of
#    sum type '$str'" until then).  127.292: arm labels must name a type or
#    variant in scope (E3011 at name resolution), so the later arms are real
#    names ($err, $str) that still name no variant of a str.
expect "2-not-a-variant" "$(e9005 'm=main;i=str:std.str;
f=pick(s:$str):i64{
  <mt str.lower(s) {$ok:w 1;$err:w 2;$str:w 3}
};
f=main():i64{<pick("x")};')" "1 0 match has 3 arms, but the matched value is a 'str', not a sum type, so '\$ok' names no variant of it"


echo "C373: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
