#!/usr/bin/env bash
# C642 -- `v.len` on a std.vec handle is E4025 naming `vec.len(v)` (127.259).
#
# The spec gives the `.len` property to arrays and maps only; any other
# receiver falls through to struct field lookup and E4025
# (docs/spec/semantics.md 9.1).  A std.vec handle is an imported `Vec`
# record, so `v.len` stays E4025 -- accepting it would extend the language
# past the spec.  What was wrong is the message: "struct 'Vec' has no field
# 'len'" says nothing about `vec.len`, the function that answers it.  When an
# imported module exports `<alias>.<name>` taking exactly the receiver's
# record, the E4025 message now names that call.  No `fix` field (127.243:
# E4025 carries no advice fix).
#
# Negative control: TKC=<2aa48c88's toke> -- cases 1-3 fail (the message ends
# at "has no field 'len'").
#
# Story: 127.259

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

expect "1 v.len on a std.vec handle names vec.len(v)" \
    "$(diags 'm=q;i=vec:std.vec;f=main():i64{let v=vec.new();let w=vec.push(v;3);<w.len as i64};')" \
    "E4025|struct 'Vec' has no field 'len': 'len' is a function of the module, not a field -- call vec.len(w)|ABSENT"
expect "2 the alias the program chose is the one named" \
    "$(diags 'm=q;i=vv:std.vec;f=main():i64{let v=vv.new();<v.len as i64};')" \
    "E4025|struct 'Vec' has no field 'len': 'len' is a function of the module, not a field -- call vv.len(v)|ABSENT"
expect "3 another one-argument member (pop) is named the same way" \
    "$(diags 'm=q;i=vec:std.vec;f=main():i64{let v=vec.new();<v.pop};')" \
    "E4025|struct 'Vec' has no field 'pop': 'pop' is a function of the module, not a field -- call vec.pop(v)|ABSENT"
expect "4 a name the module does not export stays the plain E4025" \
    "$(diags 'm=q;i=vec:std.vec;f=main():i64{let v=vec.new();<v.size as i64};')" \
    "E4025|struct 'Vec' has no field 'size'|ABSENT"
expect "5 vec.len(v) itself is clean" \
    "$(diags 'm=q;i=vec:std.vec;f=main():i64{let v=vec.new();<vec.len(v)};')" "rc=0"
expect "6 .len on an array is unchanged" \
    "$(diags 'm=q;f=main():u64{let a=@(1;2;3);<a.len};')" "rc=0"

echo "C642: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
