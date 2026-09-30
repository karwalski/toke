#!/usr/bin/env bash
# C291_import_cap_at_check.sh — the import cap is caught at --check and names
# the count, the limit and how to raise it (story 138.15, loke T-56).
#
# Codegen's prepass_imports refuses the (max_imports+1)th import with
# "compiler limit exceeded: too many imports" -- no count, no limit, no flag
# -- and only when a binary is built; `--check` was clean.  names.c now counts
# the imports the way prepass_imports does and reports E9010 at the first
# import over the cap, with expected=<limit>, got=<count> and a fix naming
# --max-imports / max_imports.  --max-imports raises it at --check too.
#
# Negative control: TKC=<main's toke> -- cases 1 and 2 fail (rc=0).
#
# Story: 138.15 (T-56)

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
# 33 std imports: one over the default cap of 32
MODS="io str json math file sys time env log csv html svg canvas chart image path http net os mem task crypto encoding encrypt auth ws sse router template dataframe analytics ml collections"
prog() { { printf 'm=q;\n'; n=0; for m in $MODS; do n=$((n+1)); printf 'i=a%d:std.%s;\n' "$n" "$m"; done; printf 'f=main():i64{<0};\n'; } > "${WORK}/p.tk"; }
prog
out() { "${TKC}" --check "$@" "${WORK}/p.tk" 2>&1 | python3 -c '
import sys, json
out = []
for l in sys.stdin:
    try: d = json.loads(l)
    except Exception: continue
    out.append("%s|%s|%s|%s" % (d.get("error_code"), d.get("expected"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "clean")'; }
expect "1 33 imports against the default cap of 32" \
    "$(out)" "E9010|32|33|raise the limit: pass --max-imports=33, or set max_imports = 33 in tkc.toml"
expect "2 the check fails, not just prints" \
    "$("${TKC}" --check "${WORK}/p.tk" >/dev/null 2>&1; echo "rc=$?")" "rc=1"
# 138.15 (T-54): these 33 std modules declare 75 struct types between them,
# over what was the default max_structs of 64 -- that cap is met at --check
# too now (before, 11 of the records were dropped silently), so this raised
# both.  127.250 made the default 1024; the explicit flag is kept.
expect "3 --max-imports raises it at --check" "$(out --max-imports=40 --max-structs=75)" "clean"
MODS="io str"; prog
expect "4 a module under the cap is clean" "$(out)" "clean"

echo "C291: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
