#!/usr/bin/env bash
# C1371_struct_literal_duplicate_field.sh — a struct literal gives each
# field once (story 127.366).
#
# `t=$r{a:i64;b:i64}; let r=$r{a:1;b:2;a:3}` passed --check and the last
# write won (`r.a` was 3).  A repeated field is now E4078 at the repeated
# field init, naming it, with no `fix`: which value was meant, or which other
# field the second write was meant for, is not decidable.
#
# Negative control: TKC=<main's toke> -- every E4078 case fails (rc=0).
#
# Story: 127.366

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
# code|expected|got|col|fix-or-ABSENT of each error joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    (cd "${WORK}" && "${TKC}" --check p.tk) > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("severity") != "error": continue
    out.append("%s|%s|%s|%s|%s" % (d.get("error_code"), d.get("expected"), d.get("got"),
                                d.get("pos", {}).get("col"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

# 1. The 127.366 repro.
expect "1 repro: a repeated field at its second occurrence" \
    "$(diags 'm=a;t=$r{a:i64;b:i64};f=main():i64{let r=$r{a:1;b:2;a:3};<r.a};')" \
    "E4078|each field given once|a|53|ABSENT"
# 2. A repeat standing in for an omitted field: both are reported.
expect "2 repeat plus omission" \
    "$(diags 'm=a;t=$r{a:i64;b:i64};f=main():i64{let r=$r{a:1;a:3};<r.a};')" \
    "E4078|each field given once|a|49|ABSENT / E4077|every declared field|'b'|43|ABSENT"
# 3. Three writes to one field: each repeat is reported.
expect "3 a field given three times" \
    "$(diags 'm=a;t=$r{a:i64;b:i64};f=main():i64{let r=$r{a:1;b:2;a:3;a:4};<r.a};')" \
    "E4078|each field given once|a|53|ABSENT / E4078|each field given once|a|57|ABSENT"
# 4. A std .tki record.
expect "4 std record with a repeated field" \
    "$(diags 'm=a;i=tm:std.time;f=main():i64{let p=$timeparts{year:2026;month:1;day:1;hour:0;min:0;sec:0;day:2};<p.month};')" \
    "E4078|each field given once|day|92|ABSENT"
# 5. Control: distinct fields that share a prefix are not repeats.
expect "5 distinct fields stay clean" \
    "$(diags 'm=a;t=$r{a:i64;ab:i64;abc:i64};f=main():i64{let r=$r{abc:3;a:1;ab:2};<r.a};')" "rc=0"

echo "C1371: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
