#!/usr/bin/env bash
# C2057_e4026_no_advice_fix.sh -- E4026 (wrong argument count) carries no
# `fix` on any path; the advice that was the fix is in the message (138.51).
#
# THE BUG.  E4026's `fix` was advice -- "match the call to the declaration, or
# reconcile the interface with the implementation if they disagree", "check
# the function signature for the correct number of parameters", "match the
# call to the method's signature; ..." -- not a repair that is right in every
# case it is emitted (AGENTS §6).  The D-series (D710-D713) covers the paths a
# single --check file reaches; this script covers the one that needs a second
# module: an alias call judged against a user module's generated .tki, whose
# message names the declared signature.
#
# WHAT IS ASSERTED
#   1. a user module's .tki declares two(a:i64;b:i64); `b.two(1)` is E4026
#      with expected 2, got 1, and no `fix` key in the JSON record
#   2. its message names the declaration and ends with the advice
#   3. every E4026 the five-path probe emits (plain, receiver, std alias,
#      std sub-path, user alias) has no `fix`
#
# NEGATIVE CONTROL: TKC=<a tkc before 138.51> -- 1, 2 and 3 FAIL.
#
# Story: 138.51
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2057_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2057: E4026 has no advice fix (138.51)"
echo "------------------------------------------------------------------------"

printf 'm=b;\nf=two(a:i64;b:i64):i64{<a+b};\n' >b.tk
"${TKC}" --emit-interface --check b.tk >/dev/null 2>&1
if [ ! -f b.tki ]; then fail "b.tki was not written"; echo "Results: ${PASS} passed, ${FAIL} failed"; exit 1; fi

printf 'm=mn;\ni=b:b;\nf=main():i64{<b.two(1)};\n' >mn.tk
"${TKC}" --check -I . mn.tk >/dev/null 2>mn.err

# recs FILE -- one line per E4026 record: expected|got|fix-or-<absent>|message
recs() {
    python3 -I - "$1" <<'PY'
import json, sys
for line in open(sys.argv[1], encoding="utf-8", errors="replace"):
    line = line.strip()
    if not line.startswith("{"): continue
    try: r = json.loads(line)
    except ValueError: continue
    if r.get("error_code") != "E4026": continue
    print("|".join([r.get("expected", ""), r.get("got", ""),
                    r["fix"] if "fix" in r else "<absent>", r.get("message", "")]))
PY
}
r1="$(recs mn.err)"
IFS='|' read -r e1 g1 f1 m1 <<<"${r1}"
if [ "${e1}" = 2 ] && [ "${g1}" = 1 ] && [ "${f1}" = "<absent>" ]; then
    pass "1 b.two(1) against b.tki: E4026 expected 2 got 1, no fix"
else fail "1 got '${r1:-no E4026}'"; fi
case "${m1}" in
    *"the interface declares"*"the call passes 1; match the call to the declaration, or reconcile the interface with the implementation if they disagree")
        pass "2 the message names the declaration and ends with the advice" ;;
    *) fail "2 message '${m1}'" ;;
esac

cat >all.tk <<'TK'
m=all;
i=b:b;
i=json:std.json;
i=http:std.http;
f=add(a:i64;b:i64):i64{<a};
f=main():i64{
  let a="xx";
  let n=a.len(1;2);
  let p=json.parse("a";"b");
  let q=http.res.ok("a";"b";"c");
  <add(1)+b.two(1)
};
TK
"${TKC}" --check -I . all.tk >/dev/null 2>all.err
n="$(recs all.err | wc -l | tr -d ' ')"
nf="$(recs all.err | awk -F'|' '$3 != "<absent>"' | wc -l | tr -d ' ')"
if [ "${n}" -ge 4 ] && [ "${nf}" = 0 ]; then
    pass "3 ${n} E4026 records over the call paths, none with a fix"
else fail "3 ${n} E4026 records, ${nf} with a fix"; recs all.err | sed 's/^/      | /'; fi

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
