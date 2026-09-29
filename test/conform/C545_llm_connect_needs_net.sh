#!/usr/bin/env bash
# C545_llm_connect_needs_net.sh — std.llm's TCP connect is capability-gated:
# a --cap-enforce binary with no net grant cannot reach the model server
# (story 121.42 item 2).
#
# THE DEFECT, measured on main (d3a083b8):
#   llm.c's tcp_connect -- the one connect behind llm.chat, chatstream,
#   complete and llmtool's chatwithtools/submitresult -- had no
#   TK_REQUIRE(TK_CAP_NET).  A binary built --cap-enforce and granted
#   nothing connected to the server anyway.  scripts/check_capability_sinks.py
#   carried the 15 paths as dated known-ungated lines.
#
# NOW: tcp_connect requires the net grant; the 15 skiplist lines are gone
# (the static gate is C470; removing the TK_REQUIRE fails it with
# "15 ungated").  This is the runtime half: a local listener counts
# connections; the probe calls llm.chat against it.
#   1. no grant: exit 1, CAP001 'net', and no connection reached the listener
#   2. --allow-net: the connection reaches the listener
#
# Negative control: C545_STDLIB=<tree>/src/stdlib builds against another tree
# (its stdlib/*.tki beside it, as `git archive main src/stdlib stdlib` gives);
# main's connects with no grant (4 connections, exit 0).
#
# Story: 121.42
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C545_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "  SKIP: python3 runs the listener"; echo "C545: 0 passed, 0 failed (skipped)"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c545_XXXXXX")"
LPID=""
trap '[ -n "${LPID}" ] && { kill "${LPID}" 2>/dev/null; wait "${LPID}" 2>/dev/null; }; rm -rf "${WORK}"' EXIT
cd "${WORK}"
echo "C545: std.llm connects only with the net grant (121.42)"
echo "--------------------------------------------------------"

# A listener on an ephemeral port: writes the port, then counts connections
# (each is closed at once, so the client sees an error and exits).
cat > l.py <<'EOF'
import socket, sys
s = socket.socket(); s.bind(("127.0.0.1", 0)); s.listen(8)
open(sys.argv[1], "w").write(str(s.getsockname()[1]))
n = 0
while True:
    c, _ = s.accept(); n += 1
    open(sys.argv[2], "w").write(str(n)); c.close()
EOF
python3 l.py port count & LPID=$!
for _ in $(seq 1 50); do [ -s port ] && break; sleep 0.1; done
PORT="$(cat port 2>/dev/null)"
[ -n "${PORT}" ] || { fail "listener did not start"; echo "C545: ${PASS} passed, ${FAIL} failed"; exit 1; }

cat > p.tk <<EOF
m=p;
i=llm:std.llm;
i=io:std.io;
f=main():i64{
  let c=llm.client("http://127.0.0.1:${PORT}/v1";"k";"m");
  let msgusr=\$llmmsg{role:"user";content:"hi"};
  let msgs=@(msgusr;msgusr);
  let r=mt llm.chat(c;msgs) {\$ok:x "ok";\$err:e "err"};
  io.println(r);
  <0
};
EOF
if ! TKC_STDLIB_DIR="${S}" "${TKC}" --cap-enforce p.tk --out p > p.cl 2>&1; then
    fail "probe does not compile: $(head -c 400 p.cl)"
    echo ""; echo "C545: ${PASS} passed, ${FAIL} failed"; exit 1
fi
count() { cat count 2>/dev/null || echo 0; }

rc=0; ./p > o1 2> e1 || rc=$?
n1="$(count)"
if [ "${rc}" -eq 1 ] && /usr/bin/grep -a -q "CAP001: capability 'net'" e1 && [ "${n1}" = 0 ]; then
    pass "1 no grant: exit 1, CAP001 'net', no connection reached the listener"
else
    fail "1 no grant: exit ${rc}, connections ${n1}, stderr: $(head -c 200 e1) stdout: $(head -c 100 o1)"
fi
rc=0; ./p --allow-net > o2 2> e2 || rc=$?
for _ in $(seq 1 30); do [ "$(count)" != "${n1}" ] && break; sleep 0.1; done
n2="$(count)"
if [ "${n2}" != "${n1}" ] && ! /usr/bin/grep -a -q "CAP001" e2; then
    pass "2 --allow-net: the connection reached the listener (exit ${rc}, printed $(head -c 20 o2))"
else
    fail "2 --allow-net: exit ${rc}, connections ${n2}, stderr: $(head -c 200 e2)"
fi
echo ""
echo "C545: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
