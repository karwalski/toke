#!/usr/bin/env bash
# C434_regex_literal_located.sh — E4075 points at the offending byte of a
# literal pattern, under any alias, in both compile paths (story 137.9).
#
# Owner decision 2026-09-22: reject unsupported regex syntax at compile time.
# src/regex_check.c compiles each literal pattern of a std.str regex call with
# the runtime's own engine and reports E4075 at the byte inside the literal.
#
# WHAT IS ASSERTED
#   1. --check, alias `t` for std.str, pattern "\\s*?" preceded by a 2-byte
#      source escape: E4075 at line 4, column of the `?` (the escape is
#      mapped back, so the column is the source column, not the decoded one)
#   2. an error on a later line of a multi-line program is located there
#   3. the multi-module build path (toke a.tk b.tk -o out) reports it too and
#      builds nothing
#   4. a computed pattern is not judged at compile time (it is RT008's job):
#      --check exits 0
#
# Negative control: TKC=<main's toke> -- 1, 2 and 3 fail (exit 0, no E4075;
# measured on Darwin 25.6).
#
# Story: 137.9
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c434_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
# first E4075 as "rc|line:col" from the JSON diagnostics
first() { python3 -c '
import json,sys
rc=sys.argv[1]; hit=""
for l in open(sys.argv[2]):
    try: d=json.loads(l)
    except ValueError: continue
    if d.get("error_code")=="E4075" and not hit:
        hit="%d:%d" % (d["pos"]["line"], d["pos"]["col"])
print(rc+"|"+hit)' "$1" "$2"; }

cat > a.tk <<'TOKE'
m=main;
i=t:std.str;
f=main():i64{
  <t.countre("a b";"\\s*?")
};
TOKE
"${TKC}" --check a.tk > a.log 2>&1; expect "1-located, alias t" "$(first $? a.log)" "1|4:25"

cat > b.tk <<'TOKE'
m=main;
i=str:std.str;
f=main():i64{
  let a=str.countre("a b";"[a-z]+");
  let b=str.containsre("a b";"(?i)x");
  <str.countre("abc";"a{2,1}")
};
TOKE
"${TKC}" --check b.tk > b.log 2>&1; expect "2-later line" "$(first $? b.log)" "1|6:24"

cat > lib.tk <<'TOKE'
m=lib;
i=str:std.str;
f=hasws(x:$str):i64{<str.containsre(x;"x\\1")};
TOKE
cat > main.tk <<'TOKE'
m=main;
i=lib:lib;
f=main():i64{<lib.hasws("a b")};
TOKE
"${TKC}" lib.tk main.tk -o prog > m.log 2>&1; rc=$?
expect "3-multi-module build" "$(first $rc m.log)|$([ -e prog ] && echo built || echo none)" "1|3:41|none"

cat > d.tk <<'TOKE'
m=main;
i=str:std.str;
f=main():i64{
  let p:str=str.concat("<.*";"?>");
  <str.countre("a";p)
};
TOKE
"${TKC}" --check d.tk > d.log 2>&1; expect "4-computed not judged" "$(first $? d.log)" "0|"
echo "C434: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
