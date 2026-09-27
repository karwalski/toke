#!/usr/bin/env bash
# C325_unterminated_comment.sh — a comment still open at end of file is E1007,
# never a silent end of input (story 138.10, loke handoff T-27).
#
# THE DEFECT.  Comments nest (toke-spec-v0.3.md §8.10: `(* outer (* inner *) *)`
# is one sequence), so the `(*` in `(* reads COUNT(*) from the table *)` opens a
# second level, the `*)` meant for the outer comment closes that one, and the
# outer comment ran to end of file.  The lexer stopped without a word: every
# declaration after the comment vanished, the executable build said only "no
# main function", and `--emit-llvm` exited 0 with the functions missing.
#
# DECIDED.  Nesting stays (it is normative).  A comment open at end of file is
# E1007 at the outermost `(*`, naming the nested `(*` that took its `*)` or the
# innermost level still open.  No `fix` field: where the missing `*)` goes, or
# whether the inner `(*` should go instead, is not decidable (AGENTS.md §6).
#
# WHAT IS ASSERTED:
#   1. loke's T-27 shape: exit 1, one E1007, at 2:1, naming the `(*` at 2:15,
#      no fix field -- through the build, --emit-llvm and --check
#   2. three levels open: the message counts them and names the innermost
#   3. nested comments that do close still compile, and the code after them
#      runs (the spec's nesting is kept)
#   4. `(*` inside a string literal is not a comment
#
# Negative control: TKC=<main c15dd11 build> fails 1 (and 2).
#
# Story: 138.10
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c325_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C325: an unterminated comment is E1007 (138.10 / T-27)"
echo "--------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
# field FILE KEY -> the JSON value of KEY in the first E1007 record
field() { awk '/"error_code":"E1007"/{print; exit}' "$1" | python3 -c '
import json,sys
l=sys.stdin.read().strip()
if not l: print("<no E1007>"); sys.exit()
d=json.loads(l); k=sys.argv[1]
v=d["pos"][k] if k in ("line","col") else d.get(k,"<absent>")
print(v)' "$2"; }
ncodes() { grep -a -c "\"error_code\":\"$2\"" "$1"; }

printf 'm=q;\n(* reads COUNT(*) from the table *)\nf=survived():i64{ <1 };\nf=main():i64{ <survived() };\n' > t27.tk

"${TKC}" --out t27_bin t27.tk > build.log 2>&1; rc=$?
expect "T-27 build: exit 1" "${rc}" "1"
expect "T-27 build: exactly one E1007" "$(ncodes build.log E1007)" "1"
expect "T-27 build: no 'no main function' stand-in (E9023)" "$(ncodes build.log E9023)" "0"
expect "E1007 is at the outer '(*' (line)" "$(field build.log line)" "2"
expect "E1007 is at the outer '(*' (col)" "$(field build.log col)" "1"
expect "E1007 names the nested '(*' at 2:15" \
    "$(field build.log message | grep -c "the '(\*' at 2:15 inside it opened a nested comment")" "1"
expect "E1007 carries no fix field" "$(field build.log fix)" "<absent>"

"${TKC}" --emit-llvm --out t27.ll t27.tk > ll.log 2>&1; rc=$?
expect "T-27 --emit-llvm: exit 1, not 0 with the functions missing" "${rc}" "1"
expect "T-27 --emit-llvm: E1007" "$(ncodes ll.log E1007)" "1"

"${TKC}" --check t27.tk > chk.log 2>&1; rc=$?
expect "T-27 --check: exit 1 with E1007" "${rc} $(ncodes chk.log E1007)" "1 1"

printf 'm=q;\nf=main():i64{ <0 };\n(* a (* b (* c\n' > deep.tk
"${TKC}" --check deep.tk > deep.log 2>&1
expect "three levels open: counted, innermost named" \
    "$(field deep.log message | grep -c "3 levels are open at end of file, the innermost opened by the '(\*' at 3:11")" "1"

printf 'm=q;\n(* one *)\n(* outer (* inner *) still outer *)\nf=four():i64{ <4 };\nf=main():i64{ <four() };\n' > nest.tk
if "${TKC}" --out nest_bin nest.tk > nest.log 2>&1; then
    ./nest_bin; expect "closed nested comments: the code after them runs" "$?" "4"
else expect "closed nested comments compile" "failed: $(head -c 200 nest.log)" "compiles"; fi

printf 'm=q;\ni=io:std.io;\nf=main():i64{ io.println("(* not a comment"); <5 };\n' > str.tk
if "${TKC}" --out str_bin str.tk > str.log 2>&1; then
    ./str_bin > str.out; expect "'(*' in a string literal is text: exit 5" "$?" "5"
    expect "'(*' in a string literal is printed" "$(cat str.out)" "(* not a comment"
else expect "'(*' in a string literal compiles" "failed: $(head -c 200 str.log)" "compiles"; fi

echo "--------------------------------------------------------"
echo "C325: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
