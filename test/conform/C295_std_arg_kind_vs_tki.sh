#!/usr/bin/env bash
# C295_std_arg_kind_vs_tki.sh — a std-module argument of another KIND than
# its typed .tki parameter is E4031 (story 138.3 (c)).
#
# Nothing compared a std call's argument types with the interface:
# `str.len(5)`, `str.len(true)`, `http.serveworkers(hc;2)` and
# `http.serveon(8080;"127.0.0.1")` all passed --check.  Judged on
# hand-written records only (a "gen":"abi" record's types are placeholders),
# by representation class -- str / integer-or-bool / float / array / map /
# record -- never by width: `u64`/`u8` parameters that receive an i64 are the
# corpus-wide norm and the glue takes an i64 slot.  A parameter spelled `i64`
# is not judged (it is also the placeholder spelling), nor is json.enc, which
# codegen redirects by argument type.
#
# Negative control: TKC=<main's toke> -- cases 1-4 and 8 fail (rc=0);
# case 11 fails on the build before the C-string exemption (E4031).
#
# Story: 138.3 (c)

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

expect "1 str.len(5)" \
    "$(diags 'm=q;i=s:std.str;f=main():i64{<s.len(5) as i64};')" "E4031|i64|ABSENT"
expect "2 str.len(true)" \
    "$(diags 'm=q;i=s:std.str;f=main():i64{<s.len(true) as i64};')" "E4031|bool|ABSENT"
expect "3 http.serveworkers(client;2)" \
    "$(diags 'm=q;i=h:std.http;f=main():i64{let hc=h.client("http://x");<h.serveworkers(hc;2)};')" "E4031|httpclient|ABSENT"
expect "4 http.serveon(port;host) -- arguments swapped" \
    "$(diags 'm=q;i=h:std.http;f=main():i64{<h.serveon(8080;"127.0.0.1")};')" "E4031|i64|ABSENT / E4031|str|ABSENT"
expect "5 an i64 into a u64 parameter is not judged" \
    "$(diags 'm=q;i=h:std.http;f=main():i64{let p=8080;<h.serveworkers(p;2)};')" "rc=0"
expect "6 json.enc takes any kind (redirected by codegen)" \
    "$(diags 'm=q;i=j:std.json;f=main():i64{let s=j.enc(5);let t=j.enc(true);<0};')" "rc=0"
expect "7 matching kinds stay clean" \
    "$(diags 'm=q;i=s:std.str;i=h:std.http;f=main():i64{let n=s.len("ab");<h.serveon("127.0.0.1";8080)};')" "rc=0"

# LH: a non-str std call result handed to the print family segfaulted.
expect "8 io.println(str.len(..)) -- a u64 where a str is read" \
    "$(diags 'm=q;i=io:std.io;i=str:std.str;f=main():i64{io.println(str.len("abc"));<0};')" "E4031|u64|wrap the argument in a string interpolation"
expect "9 io.println(str.trim(..)) stays clean" \
    "$(diags 'm=q;i=io:std.io;i=str:std.str;f=main():i64{io.println(str.trim(" a "));<0};')" "rc=0"

# A record parameter receiving an integer is a handle (std callbacks hand
# their record as an i64 -- conform C022's tls.listen handler).
expect "10 an i64 handle into a record parameter is not judged" \
    "$(diags 'm=q;i=tls:std.tls;f=onconn(conn:i64):i64{tls.close(conn);<0};f=main():i64{<0};')" "rc=0"

# http.stream's .tki and glue now agree (121.53 (2), Batch Y1): it takes the
# httpclient it publishes, so a URL str where the record is declared is judged.
expect "11 a str where http.stream declares its httpclient record is E4031" \
    "$(diags 'm=q;i=h:std.http;f=main():i64{let s=mt h.stream("http://x") {$ok:v 1;$err:e 0};<s};')" "E4031|str|ABSENT"

echo "C295: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
