#!/usr/bin/env bash
# C053_nested_interp_check.sh — an interpolation nested inside `\(..)` is
# E4073 at --check, where it used to pass --check and fail the build
# (story 127.47).
#
# THE DEFECT. `"\(tag("\(n)"))"`: at 39b6cfd `--check` exits 0 and the build
# exits 2 with E2002 x2 + E2004 reported at line 1 col 28 — an offset in the
# lowering's synthetic wrapper buffer, pointing at an innocent line of the
# real file. The lowering (llvm.c) finds the end of each `\(..)` by counting
# parens while stepping over every `\x` escape pair, so the `(` of a nested
# `\(` is not counted and its `)` is: it sub-parses a slice that stops short.
# The lexer counts every paren, which is what delimited the literal.
#
# WHAT IS ASSERTED:
#   1. The nested shape is E4073 at --check, at the string's real line, with
#      no fix (lifting into a `let` can reorder side effects).
#   2. The build now stops on the same E4073 instead of the wrapper E2002s.
#   3. Correct programs stay clean and print right: the lifted control, an
#      interpolated call taking a plain string literal, an escaped backslash
#      followed by a paren (`\\(` is not an interpolation), parens inside an
#      interpolated expression.
#
# Negative control: TKC built from 39b6cfd — case 1 checks clean (red).
#
# Story: 127.47

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
codes() { "${TKC}" "$@" 2>&1 | awk '/"severity":"error"/ && match($0,/"error_code":"[A-Z][0-9]+"/){printf "%s ",substr($0,RSTART+14,RLENGTH-15)}'; }
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

cat > "${WORK}/n.tk" <<'EOF2'
m=t;
i=io:std.io;
f=tag(s:str):str{<"<\(s)>"};
f=main():i64{
  let n=5;
  io.println("\(tag("\(n)"))");
  <0
};
EOF2
# ── 1. --check sees it ───────────────────────────────────────────────────
expect "1-nested-is-E4073" "$(codes --check "${WORK}/n.tk")" "E4073 "
expect "1-real-line" "$("${TKC}" --check "${WORK}/n.tk" 2>&1 | grep -o '"line":[0-9]*' | head -1)" '"line":6'
expect "1-no-fix" "$("${TKC}" --check "${WORK}/n.tk" 2>&1 | grep '"error_code"' | grep -vc '"fix":' || true)" "1"
# ── 2. the build stops on the same diagnostic ────────────────────────────
expect "2-build-E4073" "$(codes "${WORK}/n.tk" -o "${WORK}/n")" "E4073 "

# ── 3. correct programs ──────────────────────────────────────────────────
cat > "${WORK}/g.tk" <<'EOF2'
m=t;
i=io:std.io;
f=tag(s:str):str{<"<\(s)>"};
f=main():i64{
  let n=5;
  let t="\(n)";
  io.println("\(tag(t)) \(tag("x")) a\\(b) \((n+1)*2)");
  <0
};
EOF2
expect "3-correct-clean" "$(codes --check "${WORK}/g.tk")" ""
"${TKC}" "${WORK}/g.tk" -o "${WORK}/g" >/dev/null 2>&1
expect "3-correct-runs" "$("${WORK}/g" 2>&1)" '<5> <x> a\(b) 12'

echo "C053: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
