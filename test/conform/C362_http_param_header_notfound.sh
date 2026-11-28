#!/usr/bin/env bash
# C362_http_param_header_notfound.sh — http.param / http.header take $err when
# the request has no such param / header, as documented (story 135.28 item 1).
#
# THE DEFECT.  Both are `str!httprouteerr`, and http.md has always said a
# missing name is `$notfound`; the glue returned "" instead, so the $err arm
# was dead and a handler could not tell a missing param from an empty one.
#
# THE FIX (src/stdlib/tk_web_glue.c).  A miss returns 0 and stores an
# httprouteerr {1 = $notfound, msg} box in tk_current_error; a hit clears it.
# A value that is present and empty is still "" -- non-null, so $ok "".
# http.reqparam / http.reqheader (plain str) still return "" for a miss.
#
# WHAT IS ASSERTED:
#   1. through http.mockreq: present param / header -> $ok value; missing ->
#      $err; present-but-empty header -> $ok ""; reqparam/reqheader of a
#      missing name -> ""
#   2. the same handler under a real server gives the same answers
#   3. NEGATIVE CONTROL -- the $ok arms are not dead either: the present
#      names come back as their values, so "always $err" cannot pass
#
# Not asserted, reported: the $err payload cannot be matched on
# (`mt e {$notfound:m ...}` is E9005) until llvm.c lists tk_http_param_w /
# tk_http_header_w as slot wrappers; the glue already stores the box.
#
# Negative control of the suite: C362_STDLIB=<dir> builds against another
# stdlib tree.  Against main 73f7466 cases 1 and 2 fail (every missing name
# is $ok "").
#
# Story: 135.28 (item 1), 127.565
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
. "${REPO_ROOT}/test/conform_timing.sh"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C362_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0; SRV=""
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c362_XXXXXX")"
trap 'rm -rf "${WORK}"; [ -n "${SRV}" ] && kill "${SRV}" 2>/dev/null' EXIT
cd "${WORK}"
echo "C362: http.param / http.header take \$err when missing (135.28)"
echo "------------------------------------------------------------"

expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

HANDLER='
f=look(req:$req):$res{
  let a=mt http.param(req; "id") {$ok:v v; $err:e "ERR"};
  let b=mt http.param(req; "nope") {$ok:v v; $err:e "ERR"};
  let c=mt http.header(req; "x-key") {$ok:v v; $err:e "ERR"};
  let d=mt http.header(req; "x-none") {$ok:v v; $err:e "ERR"};
  let e=mt http.header(req; "x-empty") {$ok:v str.concat("["; str.concat(v; "]")); $err:e "ERR"};
  let f=str.concat("["; str.concat(http.reqparam(req; "nope"); str.concat("]["; str.concat(http.reqheader(req; "x-none"); "]"))));
  <http.resnew(200; str.concat(a; str.concat(" "; str.concat(b; str.concat(" "; str.concat(c; str.concat(" "; str.concat(d; str.concat(" "; str.concat(e; str.concat(" "; f)))))))))))
};'
WANT="7 ERR kv ERR [] [][]"

cat > mock.tk <<TK
m=mock;
i=http:std.http;
i=io:std.io;
i=str:std.str;
${HANDLER}
f=main():i64{
  let r=look(http.mockreq("GET"; "/u/7"; @(@("X-Key"; "kv"); @("X-Empty"; "")); ""; @(@("id"; "7"))));
  io.println(http.resbody(r));
  <0
};
TK
if "${TKC}" --diag-text --allow-all --out mock mock.tk > mock.log 2>&1; then got="$(./mock 2>&1) rc=$?"
else got="compile failed: $(grep -m1 '^error' mock.log)"; fi
expect "1  through http.mockreq" "${got}" "${WANT} rc=0"

cat > srv.tk <<TK
m=srv;
i=http:std.http;
i=str:std.str;
${HANDLER}
f=main():i64{
  http.gethandler("/u/:id"; &look);
  http.serveon("127.0.0.1"; 0);
  <0
};
TK
if ! "${TKC}" --diag-text --allow-all --out srv srv.tk > srv.log 2>&1; then
    expect "2  server builds" "$(grep -m1 '^error' srv.log)" ""
else
    ./srv >/dev/null 2>srv.err & SRV=$!
    PORT=""
    tk_wait_until "${SRV}" tk_listening srv.err
    PORT="$(tk_listening_port srv.err)"
    got="$(curl -s --max-time 5 -H 'X-Key: kv' -H 'X-Empty;' "http://127.0.0.1:${PORT:-1}/u/7" 2>/dev/null)"
    expect "2  under a real server" "${got}" "${WANT}"
    got="$(curl -s --max-time 5 -H 'X-Key: other' "http://127.0.0.1:${PORT:-1}/u/42" 2>/dev/null)"
    expect "3  NEGATIVE CONTROL: other values come back as themselves" "${got}" "42 ERR other ERR ERR [][]"
fi

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
