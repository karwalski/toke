#!/usr/bin/env bash
# C745 -- a type, function or constant named like an import alias (or an
# imported module's sub-namespace) gets ONE E3012 that names the alias and
# its module, with no fix (story 127.285).
#
# THE DEFECT, measured on main (8bb9f63e):
#   `i=json:std.json; t=$json{n:i64};` reported
#     E3012 "identifier 'json' is already declared in this scope"
#   at the `t=` line -- naming no earlier declaration the reader could find
#   (the alias is not a t=/f= line) -- and then a cascade on the same token:
#     E3011 "no type 'json' is declared: 'json' is a module alias, not a type"
#   with fix "did you mean 'jsonval'?": a fix that renames the user's OWN
#   type to the stdlib's (wrong in every case; AGENTS.md §6).
#
# NOW: one E3012 per clash, saying what it clashes with ("the import alias
# 'json' (i=json:std.json)" or "a sub-namespace of an imported module") and
# that import aliases share the module scope with types, functions and
# constants -- the rule as implemented.  No `fix`: renaming the declaration
# and renaming the alias are both right, depending on intent.  Which
# programs compile is unchanged; whether aliases SHOULD share that namespace
# is an open spec decision recorded on 127.285.
#
# WHAT IS ASSERTED.
#   1. type vs import alias: one E3012, alias + module named, no fix, no E3011
#   2. function vs import alias: one E3012 naming the function and alias
#   3. type vs a sub-namespace (`row` with std.db): one E3012, sub-namespace
#   4. two user types: the plain message, unchanged
#   5. controls that must still compile: a user type beside the import, and a
#      `$json`-typed hole still E3011 (127.251 kept)
#
# Negative control: TKC=<main 8bb9f63e's toke> -- cases 1-3 fail (generic
# message plus the E3011 cascade); 4 and 5 pass.
#
# Story: 127.285
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then PASS=$((PASS + 1)); echo "  PASS: $1"
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c745_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
# code|message|fix-or-ABSENT of each diagnostic joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > "${WORK}/p.tk"
    "${TKC}" --check "${WORK}/p.tk" > "${WORK}/o" 2>&1; rc=$?
    python3 - "${WORK}/o" "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s" % (d.get("error_code"), d.get("message"), d.get("fix") or "ABSENT"))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

echo "C745: import-alias clashes name the alias, no cascade (127.285)"
echo "----------------------------------------------------------------"

expect "1 type vs import alias" \
  "$(diags 'm=a;
i=json:std.json;
t=$json{n:i64};
f=main():i64{<0};')" \
  "E3012|type 'json' clashes with the import alias 'json' (i=json:std.json): import aliases share the module scope with types, functions and constants; rename the type or the alias|ABSENT"

expect "2 function vs import alias" \
  "$(diags 'm=a;
i=s:std.str;
f=s():i64{<1};
f=main():i64{<0};')" \
  "E3012|function 's' clashes with the import alias 's' (i=s:std.str): import aliases share the module scope with types, functions and constants; rename the function or the alias|ABSENT"

expect "3 type vs sub-namespace (row, std.db)" \
  "$(diags 'm=a;
i=db:std.db;
t=$row{n:i64};
f=main():i64{<0};')" \
  "E3012|type 'row' clashes with 'row', a sub-namespace of an imported module: sub-namespaces share the module scope with types, functions and constants; rename the type|ABSENT"

expect "4 two user types keep the plain message" \
  "$(diags 'm=a;
t=$box{n:i64};
t=$box{m:i64};
f=main():i64{<0};')" \
  "E3012|identifier 'box' is already declared in this scope|ABSENT"

expect "5a a user type beside the import compiles" \
  "$(diags 'm=a;
i=json:std.json;
t=$doc{n:i64};
f=main():i64{let d=$doc{n:1};<d.n};')" \
  "rc=0"

expect "5b \$json as a parameter type is still E3011 (127.251)" \
  "$(diags 'm=a;
i=json:std.json;
f=g(b:$json):i64{<0};
f=main():i64{<0};')" \
  "E3011|no type 'json' is declared: 'json' is a module alias, not a type|did you mean 'jsonval'?"

echo "----------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
