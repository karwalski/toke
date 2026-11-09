#!/usr/bin/env bash
# C1001_inplace_std_record_arg_judged.sh — a std call whose interface
# returns a RECORD, written in place as another std call's argument, is
# judged by that declared record: an `http.client(..)` where http.postheaders
# or http.geturl declares a `str` URL is E4031, as it already was when the
# client was bound by a `let` first (story 138.26, loke T-48 addendum).
#
# THE DEFECT. `http.postheaders(http.client(u);"body";hdrs)` and
# `http.geturl(http.client(u))` passed --check (at runtime nothing was sent,
# $err), while an i64 in the same slot was E4031 and `let c=http.client(u);
# http.geturl(c)` was E4031.  infer() adopts an in-place std record return
# only for a map (the emitter keys on rtypes), so the nested call reached
# check_tki_arg_types untyped and was skipped.  The declared record
# (call_decl_ret_type, the 127.66 rule) is now judged there and never stored,
# as 127.314 does for an array.
#
# Negative control: TKC built from toke main c26cbc1b -- cases 1-3 red
# (--check rc 0), 4-8 green on both.
#
# Story: 138.26

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
H='m=hr;i=io:std.io;i=s:std.str;i=http:std.http;'
# chk <body> — `--check` a main whose body is <body>; prints
# "rc=<n> <first error_code> <expected> <got>".
chk() {
    printf '%s\nf=main():i64{\n%s\n<0};\n' "$H" "$1" > "${WORK}/hr.tk"
    local out rc
    out="$("${TKC}" --check "${WORK}/hr.tk" 2>&1)"; rc=$?
    local code exp got
    code="$(printf '%s' "$out" | grep -o '"error_code":"[^"]*"' | head -1 | cut -d'"' -f4)"
    exp="$(printf '%s' "$out" | grep -o '"expected":"[^"]*"' | head -1 | cut -d'"' -f4)"
    got="$(printf '%s' "$out" | grep -o '"got":"[^"]*"' | head -1 | cut -d'"' -f4)"
    echo "rc=${rc} ${code} ${exp} ${got}"
}
U='"http://127.0.0.1:9/"'

# 1 — the filed repro (Z108 probe t48): a client in place of the URL
expect "1-client-into-postheaders" \
  "$(chk "let r=mt http.postheaders(http.client(${U});\"body\";@(@(\"X-A\";\"1\"))) {\$ok:x 0; \$err:e 1};")" \
  "rc=1 E4031 str httpclient"
# 2 — the filed repro (Z108 probe t48g)
expect "2-client-into-geturl" \
  "$(chk "let r=mt http.geturl(http.client(${U})) {\$ok:x 0; \$err:e 1};")" \
  "rc=1 E4031 str httpclient"
# 3 — another str parameter, outside std.http
expect "3-client-into-strlen" \
  "$(chk "let n=s.len(http.client(${U}));")" \
  "rc=1 E4031 str httpclient"
# 4 — control (probe t48s): an i64 in the URL slot was already E4031
expect "4-control-i64-url" \
  "$(chk "let n:i64=5; let r=mt http.postheaders(n;\"body\";@(@(\"X-A\";\"1\"))) {\$ok:x 0; \$err:e 1};")" \
  "rc=1 E4031 str i64"
# 5 — control (probe t48c): a str where httpclient is declared
expect "5-control-str-client" \
  "$(chk "let r=mt http.clientget(\"http://x/\";\"/\") {\$ok:x 0; \$err:e 1};")" \
  "rc=1 E4031 httpclient str"
# 6 — control: the let-bound client was already E4031
expect "6-control-let-bound" \
  "$(chk "let c=http.client(${U}); let r=mt http.geturl(c) {\$ok:x 0; \$err:e 1};")" \
  "rc=1 E4031 str httpclient"
# 7 — control: an in-place client where httpclient is declared is clean
expect "7-control-inplace-client" \
  "$(chk "let r=mt http.clientget(http.client(${U});\"/\") {\$ok:x 0; \$err:e 1};")" \
  "rc=0   "
# 8 — control: a str URL is clean
expect "8-control-str-url" \
  "$(chk "let r=mt http.geturl(${U}) {\$ok:x 0; \$err:e 1};")" \
  "rc=0   "

echo "C1001: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
