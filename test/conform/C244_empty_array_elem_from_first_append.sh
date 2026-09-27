#!/usr/bin/env bash
# C244_empty_array_elem_from_first_append.sh — an un-annotated `mut` empty
# array takes its element type from its first append (story 127.229 item 6).
#
# `let r=mut.@(); r=r+@(@(i;j))` printed its inner arrays as addresses with
# no diagnostic (eval 0064, 0102, 0192): the emitter renders elements by the
# checker's rtype and the element type of `@()` was unknown.  `:@@i64` fixed
# it.  The checker now infers what the first append stores -- `r=r+@(e)`,
# `r=r.append(e)`, `r=r.push(e)` -- and a later append of another type is
# E4031, as it is for an annotated binding.
#
# Negative control: TKC=<main's toke> -- cases 1-4 print addresses / the
# wrong rendering, and case 5 is clean.
#
# Story: 127.229 (item 6)

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
run() { printf '%s\n' "$1" > "${WORK}/p.tk"
    if "${TKC}" "${WORK}/p.tk" -o "${WORK}/p.bin" > "${WORK}/o" 2>&1; then
        "${WORK}/p.bin" | tr '\n' ' '
    else echo "build failed: $(head -c 200 "${WORK}/o")"; fi; }
H='m=q;i=j:std.json;'

expect "1 nested append in a loop (the eval shape)" \
    "$(run "${H}"'f=main():i64{let r=mut.@();lp(let i=0;i<2;i=i+1){lp(let k=0;k<2;k=k+1){r=r+@(@(i;k))}};j.print(r);j.print(r.get(3).get(1));<0};')" \
    "[[0,0],[0,1],[1,0],[1,1]] 1 "
expect "2 str elements via concat" \
    "$(run "${H}"'f=main():i64{let r=mut.@();r=r+@("a");r=r+@("b");j.print(r);<0};')" '["a","b"] '
expect "3 f64 elements via .append" \
    "$(run "${H}"'f=main():i64{let r=mut.@();r=r.append(1.5);j.print(r);<0};')" '[1.5] '
expect "4 bool elements via .push" \
    "$(run "${H}"'f=main():i64{let r=mut.@();r=r.push(true);j.print(r);<0};')" '[true] '
printf 'm=q;f=main():i64{let r=mut.@();r=r+@("a");r=r+@(5);<0};\n' > "${WORK}/m.tk"
expect "5 a later append of another type is E4031" \
    "$("${TKC}" --check "${WORK}/m.tk" 2>&1 | python3 -c 'import sys,json; print(",".join(json.loads(l)["error_code"] for l in sys.stdin if l.startswith("{")))')" "E4031"
expect "6 i64 elements are unchanged" \
    "$(run "${H}"'f=main():i64{let r=mut.@();r=r+@(7);r=r.append(8);j.print(r);<0};')" '[7,8] '

echo "C244: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
