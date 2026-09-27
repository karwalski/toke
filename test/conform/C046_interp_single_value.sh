#!/usr/bin/env bash
# C046_interp_single_value.sh — an interpolation joins only non-empty segments.
#
# Story: 127.50.
#
# THE DEFECT.  `"\(n)"` cost two allocations per evaluation against one for
# `n as str`, producing the same string.  The row blamed a per-value
# conversion temporary that tk_str_join_n then copies; that was wrong — there
# is one conversion per value, and a literal with k values cost k+1.  The
# second allocation in the one-value case was a tk_str_join_n call over TWO
# segments: the conversion, and the EMPTY literal segment the splitter
# recorded in front of a leading `\(`.  The same empty segment was recorded
# between two adjacent `\(a)\(b)`, widening their join by one.
#
# THE RULE.  Empty literal segments are not recorded.  A literal whose only
# non-empty segment is one interpolated value IS that value's string form: no
# join at all (a number's is its fresh conversion; a string's is the string
# itself — toke strings are immutable).  Every literal with two or more
# non-empty segments still lowers to exactly one tk_str_join_n.
#
# Asserted two ways: the IR (how many joins, and of how many parts) and the
# VALUE each program prints, including the empty-string and escape corners
# the splitter walks.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c046_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C046: an interpolation joins only non-empty segments"
echo "--------------------------------------------------"

# joins NAME EXPR EXPECTED_JOIN_LINES — the function `pat` returning EXPR must
# contain exactly EXPECTED tk_str_join_n calls, and each must be the arity
# given as "n=K" (or "none").
joins() {
    local name="$1" expr="$2" want="$3"
    cat > j.tk <<EOF
m=main;
i=io:std.io;
f=pat(n:i64;w:str):str{
  <${expr}
};
f=main():i64{
  io.println(pat(7;"ab"));
  <0
};
EOF
    rm -f j.ll
    if ! "${TKC}" --emit-llvm --out j.ll j.tk >/dev/null 2>&1 || [ ! -f j.ll ]; then
        echo "  FAIL: ${name}: --emit-llvm failed"
        FAIL=$((FAIL + 1)); return
    fi
    local got
    got="$(awk '/^define .*@main_pat\(|^define .*pat\(/{f=1} f&&/^}/{f=0} f' j.ll \
           | grep -o 'tk_str_join_n(i64 [0-9]*' | sed 's/.*i64 /n=/' | tr '\n' ' ' | sed 's/ $//')"
    [ -z "${got}" ] && got="none"
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: ${name} — joins: ${got}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected joins '${want}', got '${got}'"
        FAIL=$((FAIL + 1))
    fi
}

# runs NAME EXPECTED SOURCE — build, run, compare stdout exactly.
runs() {
    local name="$1" expected="$2" src="$3"
    printf '%s\n' "${src}" > "ok.tk"
    local out rc=0
    rm -f ok_bin
    if ! "${TKC}" --allow-all --out ok_bin "ok.tk" >compile.log 2>&1; then
        echo "  FAIL: ${name}: compile failed"
        sed 's/^/      /' compile.log | tail -3
        FAIL=$((FAIL + 1))
        return
    fi
    out="$(./ok_bin 2>&1)" || rc=$?
    if [ "${out}" = "${expected}" ]; then
        echo "  PASS: ${name} — '${expected}'"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: expected '${expected}', got '${out}' (rc ${rc})"
        FAIL=$((FAIL + 1))
    fi
}

# ── the IR: how many joins, of how many parts ────────────────────────────────
joins 'one number, no literal: "\(n)"'        '"\(n)"'            'none'
joins 'one string, no literal: "\(w)"'        '"\(w)"'            'none'
joins 'leading literal: "a\(n)"'              '"a\(n)"'           'n=2'
joins 'trailing literal: "\(n)b"'             '"\(n)b"'           'n=2'
joins 'adjacent values: "\(n)\(w)"'           '"\(n)\(w)"'        'n=2'
joins 'values and literals: "a\(n)b\(w)c"'    '"a\(n)b\(w)c"'     'n=5'
joins 'no interpolation: "plain"'             '"plain"'           'none'

# ── the values ───────────────────────────────────────────────────────────────
runs 'single number' '-42' \
'm=main;
i=io:std.io;
f=main():i64{
  let n=0-42;
  io.println("\(n)");
  <0
};'

runs 'single string, and the source is unchanged after' 'ab|ab' \
'm=main;
i=io:std.io;
i=s:std.str;
f=main():i64{
  let w="ab";
  let v="\(w)";
  io.println("\(v)|\(w)");
  <0
};'

runs 'single EMPTY string is empty' '[]' \
'm=main;
i=io:std.io;
f=main():i64{
  let e="";
  let v="\(e)";
  io.println("[\(v)]");
  <0
};'

runs 'adjacent values keep order' '7ab7' \
'm=main;
i=io:std.io;
f=main():i64{
  let n=7;
  let w="ab";
  io.println("\(n)\(w)\(n)");
  <0
};'

runs 'escapes around a value survive' 'x=3	y' \
'm=main;
i=io:std.io;
f=main():i64{
  let n=3;
  io.println("x=\(n)\ty");
  <0
};'

# A single float takes the fromfloat conversion and no join.  (A bool is not
# asserted here: `"\(b)"` prints 1/0 at HEAD, before and after this change —
# reported separately, not this story's rule.)
runs 'single float' '1.5' \
'm=main;
i=io:std.io;
f=main():i64{
  let f=1.5;
  let u="\(f)";
  io.println(u);
  <0
};'

echo "--------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
