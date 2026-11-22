#!/usr/bin/env bash
# C1906_handle_nameable_in_type_position.sh — std.process's opaque `handle`
# is nameable in type position as `$handle` or `process.$handle` (story
# 138.32, closed by the T-67 answer, 138.35).
#
# 138.32 asked whether `handle` could be written in type position.  It
# already can, in the v0.4 spelling: resolve_type falls back to the
# imported record (imported_struct_type), so `f=w(h:$handle)` and
# `f=w(h:process.$handle)` check on v3.2.0.  The bare `h:handle` is E2005
# because a v0.4 type takes the `$` sigil.  What the parameter buys a caller
# is the judgement of its call sites: a str, f64 or array argument is E4031
# (138.21), and since 138.35 an integer other than the literal 0 is too.
#
# Negative control: TKC built from v3.2.0 (ea7ddf22) -- case 6 red (rc 0:
# an integer into a $handle parameter was accepted); every other case green.
#
# Story: 138.32 (with 138.35)

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
# chk <param type> <main body> — prints "rc=<n> <first error_code> <expected> <got>"
chk() {
    printf 'm=nh;\ni=process:std.process;\nf=w(h:%s):i64{<process.wait(h) as i64};\nf=main():i64{\n%s\n<0};\n' \
        "$1" "$2" > "${WORK}/nh.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/nh.tk" 2>&1)"; rc=$?
    local code exp got
    code="$(printf '%s' "$out" | grep -o '"error_code":"[^"]*"' | head -1 | cut -d'"' -f4)"
    exp="$(printf '%s' "$out" | grep -o '"expected":"[^"]*"' | head -1 | cut -d'"' -f4)"
    got="$(printf '%s' "$out" | grep -o '"got":"[^"]*"' | head -1 | cut -d'"' -f4)"
    echo "rc=${rc} ${code} ${exp} ${got}"
}
REAL='let h=mt process.spawn(@("/usr/bin/true")) {$ok:v v;$err:e 0}; let r=w(h);'

expect "1-dollar-handle-param" "$(chk '$handle' "${REAL}")" "rc=0   "
expect "2-qualified-handle-param" "$(chk 'process.$handle' "${REAL}")" "rc=0   "
expect "3-literal-zero-accepted" "$(chk '$handle' 'let r=w(0);')" "rc=0   "
expect "4-str-refused" "$(chk '$handle' 'let r=w("abc");')" "rc=1 E4031 handle str"
expect "5-f64-refused" "$(chk '$handle' 'let r=w(1.5);')" "rc=1 E4031 handle f64"
expect "6-int-refused" "$(chk '$handle' 'let r=w(5);')" "rc=1 E4031 handle i64"
expect "7-array-refused" "$(chk 'process.$handle' 'let r=w(@(1;2));')" "rc=1 E4031 handle @(i64)"
# 8 — the bare spelling stays a parse error: a v0.4 type takes `$`
code8="$(chk 'handle' 'let r=w(0);' | cut -d' ' -f1-2)"
expect "8-bare-handle-E2005" "${code8}" "rc=1 E2005"

echo "C1906: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
