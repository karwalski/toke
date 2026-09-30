#!/usr/bin/env bash
# C560_locals_cap_one_located_e9010.sh — the max_locals caps (pointer locals,
# local types, name aliases) are reported ONCE per function, at the function,
# naming it, the limit and the count (story 127.252 item 1).
#
# Before: every local past the cap emitted its own E9010 "compiler limit
# exceeded: too many local variables" / "too many pointer locals" at line 0 --
# 12 records for 140 i64 lets, 24 for 140 string lets -- naming no function,
# no count and no limit.
#
# Now: one E9010 at the f= (or closure, or globals initialiser) that crossed
# the cap; "expected" is the limit, "got" the count.  The count is a LOWER
# bound ("at least"): a full registry stops recording, and what it does not
# record changes what the others need (case 6 shows it: shadowing reports 140
# and needs 141).  So the fix field is absent (AGENTS.md §6).
#
# --check counts a LOWER bound of these caps since the 127.252 residue
# (tkc_caps_locals_check, C586): parameters, lets and loop lets of a
# function's own body, in the same words.  For the plain functions below that
# bound already reaches the count, so cases 1-6 and 8 are now refused by the
# checker before codegen runs; codegen's own once-per-function report is
# still what cases 7 (a closure body, which the checker leaves to codegen)
# and C586's arm case see.  Case 9 used to pin "--check stays silent".
#
# Negative control: TKC=<main's toke> -- cases 1-3 and 5-8 fail (main
# emits one unlocated record per overflowing local).
#
# Story: 127.252 (1)

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
cd "${WORK}" || exit 1

# code|line|expected|got|fix of each error, joined by ' / '; "clean" if none
diags() { "${TKC}" "$@" 2>&1 | python3 -c '
import sys, json
out = []
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    if d.get("severity") != "error": continue
    out.append("%s|%s|%s|%s|%s" % (d.get("error_code"), d.get("pos", {}).get("line"),
               d.get("expected"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "clean")'; }
msg() { "${TKC}" "$@" 2>&1 | python3 -c '
import sys, json
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    if d.get("error_code") == "E9010": print(d.get("message")); break'; }

# 140 i64 lets in one function (line 2)
{ printf 'm=loc;\nf=big():i64{\n'; for i in $(seq 1 140); do printf '  let v%d=%d;\n' "$i" "$i"; done
  printf '  <v1+v140\n};\nf=main():i64{<big()};\n'; } > loc.tk
# 140 string lets: pointer locals AND local types overflow
{ printf 'm=ploc;\nf=big():i64{\n'; for i in $(seq 1 140); do printf '  let s%d="x%d";\n' "$i" "$i"; done
  printf '  <0\n};\nf=main():i64{<big()};\n'; } > ploc.tk
# one name shadowed 140 times: name aliases
{ printf 'm=sh;\nf=big():i64{\n  let v=0;\n'; for i in $(seq 1 140); do printf '  let v=v+%d;\n' "$i"; done
  printf '  <v\n};\nf=main():i64{<big()};\n'; } > sh.tk
# a closure body over the cap (the fn( is on line 3)
{ printf 'm=cl;\nf=mk(n:i64):(i64):i64{\n  <fn(x:i64):i64{\n'; for i in $(seq 1 140); do printf '    let v%d=x+%d;\n' "$i" "$i"; done
  printf '    <v1+n\n  }\n};\nf=main():i64{let g=mk(1);<g(2)};\n'; } > cl.tk
# two functions over the cap: one record each, at each
{ printf 'm=two;\nf=a():i64{\n'; for i in $(seq 1 130); do printf '  let v%d=%d;\n' "$i" "$i"; done
  printf '  <v1\n};\nf=b():i64{\n'; for i in $(seq 1 135); do printf '  let w%d=%d;\n' "$i" "$i"; done
  printf '  <w1\n};\nf=main():i64{<a()+b()};\n'; } > two.tk
BL=$((2 + 130 + 3))   # line of f=b: m, f=a, 130 lets, <v1, };

expect "1 one E9010 at the function, limit and count, no fix" \
    "$(diags --emit-llvm --out o.ll loc.tk)" "E9010|2|128|140|ABSENT"
expect "2 the message names the function, the count, the limit and the flag" \
    "$(msg --emit-llvm --out o.ll loc.tk)" \
    "compiler limit exceeded: function 'big' needs at least 140 local-variable slots and the limit is 128 (max_locals); raise it with --max-locals=N or max_locals = N in tkc.toml"
expect "3 pointer locals and local types overflowing together: still one record" \
    "$(diags --emit-llvm --out o.ll ploc.tk)" "E9010|2|128|140|ABSENT"
expect "4 raised to exactly the count: builds" \
    "$(diags --max-locals=140 --emit-llvm --out o.ll loc.tk)" "clean"
expect "5 one under the count: refused with the same count" \
    "$(diags --max-locals=139 --emit-llvm --out o.ll ploc.tk)" "E9010|2|139|140|ABSENT"
expect "6 the count is a lower bound: shadowing reports 140, and 140 is refused (why the fix is absent)" \
    "$(diags --max-locals=140 --emit-llvm --out o.ll sh.tk) ; $(diags --max-locals=141 --emit-llvm --out o.ll sh.tk)" \
    "E9010|2|140|141|ABSENT ; clean"
expect "7 a closure body: one record, at the closure" \
    "$(diags --emit-llvm --out o.ll cl.tk)" "E9010|3|128|142|ABSENT"
expect "8 two functions: one record each, at each" \
    "$(diags --emit-llvm --out o.ll two.tk)" "E9010|2|128|130|ABSENT / E9010|${BL}|128|135|ABSENT"
expect "9 --check reports the same record (lower bound, C586; see header)" \
    "$(diags --check loc.tk)" "E9010|2|128|140|ABSENT"

echo "C560: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
