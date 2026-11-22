#!/usr/bin/env bash
# C2064_llmclient_literal_refused.sh — std.llm (and std.llmtool) use only a
# client llm.client or llmtool.withtools made: a $llmclient record literal,
# and any other value, is answered as the null client is (story 138.50, the
# llmclient part).
#
# THE DEFECT.  $llmclient is a tki-record-prefix type: toke sees
# {provider; model}, and base_url, api_key, the timeouts, the retry policy
# and the usage counters follow in C (src/stdlib/llm.h).  --check accepts a
# record literal $llmclient{provider:..;model:..}, which holds the two slots
# only, so llm.complete / chat / chatstream / countokens on it read the base
# URL and API key from whatever lay past its end; an integer cast with `as`
# was read through outright.  (138.24 fixed the same shape for $httpclient.)
#
# THE FIX.  llm.client and llmtool.withtools register the clients they make
# in the runtime's handle registry (tk_runtime.c, 138.45); every call looks
# its client up and answers anything else as the null client: $err (the
# error slot set), countokens 0.  A literal cannot carry a base URL, so it is
# not given defaults the way an $httpclient literal is.
#
# Negative control: TKC=<base ee27591c tree>/toke -- the program exits 139.
#
# Story: 138.50
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2064_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2064: std.llm refuses a client it did not create (138.50)"
echo "----------------------------------------------------------------------"
cat > l.tk <<'EOF'
m=l;
i=io:std.io;
i=llm:std.llm;
i=llmtool:std.llmtool;
f=main():i64{
  let lit=$llmclient{provider:"openai";model:"m"};
  io.println("lit: count=\(llm.countokens(lit;"hello world")) complete=\(mt llm.complete(lit;"hi"){$ok:s "OK";$err:e "ERR"}) chat=\(mt llm.chat(lit;@($llmmsg{role:"user";content:"hi"})){$ok:r "OK";$err:e "ERR"})");
  let n=5;
  let fc=n as $llmclient;
  io.println("forged: count=\(llm.countokens(fc;"hello world")) complete=\(mt llm.complete(fc;"hi"){$ok:s "OK";$err:e "ERR"})");
  let c=llm.client("http://127.0.0.1:9";"";"m");
  io.println("real: count=\(llm.countokens(c;"hello world")>0)");
  let tc=llmtool.withtools(c;@());
  io.println("withtools: count=\(llm.countokens(tc;"hello world")>0)");
  <0
};
EOF
if ! "${TKC}" -o l l.tk > l.log 2>&1; then
    fail "build: $(grep -m1 error l.log | cut -c1-200)"
else
    got="$(./l --allow-all 2>&1)"; rc=$?
    want='lit: count=0 complete=ERR chat=ERR
forged: count=0 complete=ERR
real: count=true
withtools: count=true'
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then pass "literal, forged, real and withtools clients"
    else fail "rc=${rc}; got:"; printf '%s\n' "${got}" | sed 's/^/      /'; fi
fi
echo "----------------------------------------------------------------------"
echo "C2064: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
