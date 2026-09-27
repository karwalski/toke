#!/usr/bin/env bash
# C327_keyword_import_alias.sh — a keyword used as an import alias is one
# diagnostic that names the keyword (story 138.10, loke handoff T-29).
#
# THE DEFECT.  `i=sc:std.str;` drew "expected import alias, got 'sc'", which
# does not say why, and then a cascade: the parser's recovery stops on a
# statement keyword without consuming it, so the top level reported the same
# token twice more, and every `sc.len(..)` in the body was "unexpected token
# in expression" -- five errors for one mistake.
#
# DECIDED.  One E2002: "'sc' is a reserved keyword and cannot be an import
# alias".  The keyword is consumed as the alias so the declaration ends, and
# its `sc.name` uses parse as that alias instead of being reported again.  No
# `fix` field: the new name is the author's choice.
#
# WHAT IS ASSERTED:
#   1. loke's T-29 repro: exit 1, exactly one error, E2002 at 2:3, naming 'sc'
#      as a reserved keyword, no fix field
#   2. the same for lp, mt, if (expression-led keywords), as, and the literal
#      true -- one error each, with the alias used in the body
#   3. an ordinary alias still compiles and runs (negative control on the rule)
#
# Negative control: TKC=<main c15dd11 build> fails 1 and 2.
#
# Story: 138.10
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c327_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C327: a keyword import alias is one diagnostic (138.10 / T-29)"
echo "---------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
recs() { python3 -c '
import json,sys
for l in open(sys.argv[1]):
    l=l.strip()
    if not l.startswith("{"): continue
    d=json.loads(l)
    if d.get("severity")!="error": continue
    print("%s|%d:%d|%s|%s" % (d["error_code"],d["pos"]["line"],d["pos"]["col"],"fix" if "fix" in d else "nofix",d["message"]))' "$1"; }

printf 'm=s29;\ni=sc:std.str;\nf=main():i64{ <sc.len("abc") };\n' > t29.tk
"${TKC}" --out t29_bin t29.tk > t29.log 2>&1; rc=$?
R="$(recs t29.log)"
expect "T-29: exit 1" "${rc}" "1"
expect "T-29: exactly one error record" "$(printf '%s\n' "${R}" | grep -c .)" "1"
expect "T-29: E2002 at the alias (2:3), no fix field" "$(printf '%s' "${R}" | cut -d'|' -f1-3)" "E2002|2:3|nofix"
expect "T-29: names 'sc' as a reserved keyword" \
    "$(printf '%s' "${R}" | grep -c "'sc' is a reserved keyword and cannot be an import alias")" "1"

for kw in lp mt if as true; do
    printf 'm=s29;\ni=%s:std.str;\nf=main():i64{\n  %s.len("abc");\n  <%s.len("ab")\n};\n' "${kw}" "${kw}" "${kw}" > k.tk
    "${TKC}" --check k.tk > k.log 2>&1
    kind=keyword; [ "${kw}" = true ] && kind=literal
    expect "alias '${kw}': one error, naming it a reserved ${kind}" \
        "$(recs k.log | wc -l | tr -d ' ') $(recs k.log | grep -c "'${kw}' is a reserved ${kind}")" "1 1"
done

printf 'm=s29;\ni=s:std.str;\nf=main():i64{ <s.len("abc") };\n' > ok.tk
if "${TKC}" --out ok_bin ok.tk > ok.log 2>&1; then
    ./ok_bin; expect "an ordinary alias still compiles: exit 3" "$?" "3"
else expect "an ordinary alias compiles" "failed: $(head -c 200 ok.log)" "compiles"; fi

echo "---------------------------------------------------------------"
echo "C327: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
