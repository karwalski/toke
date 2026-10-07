#!/usr/bin/env bash
# C296_std_scalar_return_typed.sh — a std call carries the scalar return its
# typed .tki record declares (story 138.3 (c) / 138.11; loke T-47b, T-28,
# T-32; 127.232 (1)).
#
# stdlib returns were never typed by the checker (127.66), so the value of
# `str.nowunix()` (a str) took part in `/86400` (T-47b), the i64 from
# `str.charat` reached `str.isdigit`'s str parameter (T-28), and the str from
# `json.getval` reached `json.i64`'s Json parameter and segfaulted (T-32).
# A hand-written record's return is adopted when its spelling carries a
# representation -- str, bool, f64/f32, i64 -- never a "gen":"abi"
# placeholder, a T!E, or a width-only u64/u8 (str.len is `u64`; adopting it
# added ~670 W1001s and 15 E4031s to working corpus records).
#
# Negative control: TKC=<main's toke> -- cases 1-4 fail (rc=0).
#
# Story: 138.3 (c), 138.11, 127.232 (1)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# code|got|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}
H='m=q;i=s:std.str;i=j:std.json;i=math:std.math;'

expect "1 str.nowunix() is a str: arithmetic on it (loke T-47b)" \
    "$(diags "${H}"'f=main():i64{let a=s.nowunix();<a/86400};')" "E4031|str|ABSENT"
expect "2 in place, not only through a binding" \
    "$(diags "${H}"'f=main():i64{<s.nowunix()/86400};')" "E4031|str|ABSENT"
expect "3 str.charat's i64 into str.isdigit (loke T-28)" \
    "$(diags "${H}"'f=main():i64{let c=s.charat("a5b";1);if(s.isdigit(c)){<1};<0};')" "E4031|i64|ABSENT"
expect "4 json.getval's str into json.i64's jsonval (loke T-32)" \
    "$(diags 'm=q;i=json:std.json;f=main():i64{let o=mt json.dec("{}") {$ok:v v;$jsonerr:e $jsonval{raw:""}};let g=json.getval(o;"u");let c=mt json.i64(g;"k") {$ok:n n;$jsonerr:e 0};<c};')" "E4031|str|ABSENT"
expect "5 a u64 return is not adopted: no W1001 on s.len(x) as i64" \
    "$(diags "${H}"'f=main():i64{let n=s.len("abc");<n as i64};')" "rc=0"
expect "6 a declared bool used as a bool stays clean" \
    "$(diags "${H}"'f=main():i64{let b=s.contains("abc";"b");if(b){<1};<0};')" "rc=0"

echo "C296: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
