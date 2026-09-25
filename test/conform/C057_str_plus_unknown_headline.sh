#!/usr/bin/env bash
# C057_str_plus_unknown_headline.sh — `str + <untyped>` is still E4031, and
# its headline names what `+` requires instead of the unresolved side
# (story 127.173).
#
# THE DEFECT. At 39b6cfd, with `a` a str and `st.trim(..)` a stdlib call the
# checker does not type:
#
#     a + st.trim(" b ")   E4031 "type mismatch: expected 'str', got 'unknown'"
#     st.trim(" b ") + a   E4031 "type mismatch: expected 'unknown', got 'str'"
#
# Right to reject (`+` is numeric only, ADR-0004), unreadable as a
# diagnosis: 'unknown' is the checker's own state, not a type the author
# wrote. Same class as 127.140's "expected 'str', got 'str'".
#
# WHAT IS ASSERTED: both orders are E4031 with expected "a numeric type",
# got "str", and no 'unknown' anywhere in the headline or those fields.
#
# Negative control: TKC built from 39b6cfd — the headline check fails (red).
#
# Story: 127.173

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
fields() {
    "${TKC}" --check "$1" 2>&1 | python3 -c '
import sys, json
for line in sys.stdin:
    line = line.strip()
    if not line.startswith("{"): continue
    d = json.loads(line)
    if d.get("severity") != "error": continue
    bad = "unknown" in d.get("message","") or "unknown" in d.get("expected","") or "unknown" in d.get("got","")
    print(d["error_code"], d.get("expected"), "|", d.get("got"), "|", "BAD-HEADLINE" if bad else "ok")
'
}
for order in 'a+st.trim(" b ")' 'st.trim(" b ")+a'; do
    printf 'm=t;\ni=io:std.io;\ni=st:std.str;\nf=main():i64{\n  let a="x";\n  let r=%s;\n  io.println(r);\n  <0\n};\n' "$order" > "${WORK}/p.tk"
    expect "E4031-headline-${order}" "$(fields "${WORK}/p.tk")" "E4031 a numeric type | str | ok"
done

echo "C057: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
