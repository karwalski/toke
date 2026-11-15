#!/usr/bin/env bash
# C1864_net_resolve.sh — net.resolve(name) returns every address the name
# has (A and AAAA), and is gated by the net capability (story 138.25, owner
# answer 2.2; loke T-63 control 2).
#
# THE GAP.  std exported no general resolver (the only `resolve` in
# stdlib/*.tki was mdns.resolve), so a caller could not see the addresses a
# name has, check each against its SSRF rules, and then connect to the one
# it checked (http.withconnectaddr, C1862).
#
# NOW: net.resolve(name:$str):@($str)!$neterr -- one getaddrinfo() of any
# family, each address once as a numeric string, in the resolver's order;
# $err (a neterr {msg; code}, code 0) when the name does not resolve.  A
# lookup is network traffic: the call takes the `net` grant, as connect does.
#
# Nothing here leaves the machine: only "localhost" and address literals are
# resolved, from the hosts file.
#   1. localhost: exactly the addresses Python's getaddrinfo(localhost,
#      SOCK_STREAM) gives on this host (scope ids dropped), 127.0.0.1 among them
#   2. "127.0.0.1": @("127.0.0.1")
#   3. "::1": @("::1")
#   4. "": $err "net.resolve: empty name", code 0
#   5. built --cap-enforce, run without --allow-net: exit 1, CAP001 'net',
#      and nothing after the call ran
#
# Negative control: C1864_STDLIB=<dir> builds against another stdlib tree.
# Measured 2026-11-15: against v3.2.0's src/stdlib the program does not
# check (E4034: no neterr there).  With the TK_REQUIRE removed from
# tk_net_resolve_w: 5 passed, 1 failed -- 5 answers the addresses with no
# grant -- and scripts/check_capability_sinks.py (C470) reports
# "UNGATED: tk_net_resolve_w | net.c:net_resolve | getaddrinfo | net".
#
# Story: 138.25
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C1864_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "  PASS: $1"; PASS=$((PASS + 1))
    else echo "  FAIL: $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 gives the reference answer"; exit 0; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1864_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1864: net.resolve returns every address, needs net (138.25)"
echo "--------------------------------------------------------------"

cat > rs.tk <<'TOKE'
m=rs;
i=net:std.net;
i=io:std.io;
i=args:std.args;
f=main():i64{
  io.println("start");
  let out=mt net.resolve(args.get(1)) {
    $ok:a a;
    $err:e @("err \(e.msg) code=\(e.code)")
  };
  lp(let i=0;i<out.len;i=i+1){
    io.println(out.get(i));
  };
  <0
};
TOKE
if ! "${TKC}" --cap-enforce --out rs_bin rs.tk > build.log 2>&1; then
    echo "  FAIL: the program does not build"; sed 's/^/      /' build.log | head -6
    echo "C1864: 0 passed, 1 failed"; exit 1
fi
got() { ./rs_bin --allow-net "$1" 2>&1 | sed 1d | LC_ALL=C sort | tr '\n' ' '; }

WANT="$(python3 -c '
import socket
seen = []
for fam, _, _, _, sa in socket.getaddrinfo("localhost", None, 0, socket.SOCK_STREAM):
    a = sa[0].split("%")[0]
    if a not in seen: seen.append(a)
print(" ".join(sorted(seen)) + " ")')"
G1="$(got localhost)"
expect "1 localhost: every address the resolver has for it" "${G1}" "${WANT}"
case " ${G1}" in *" 127.0.0.1 "*) expect "1b 127.0.0.1 is among them" "yes" "yes" ;;
*) expect "1b 127.0.0.1 is among them" "${G1}" "... 127.0.0.1 ..." ;; esac
expect "2 an IPv4 literal" "$(got 127.0.0.1)" "127.0.0.1 "
expect "3 an IPv6 literal" "$(got ::1)" "::1 "
expect "4 an empty name: \$err, code 0" "$(got '')" "err net.resolve: empty name code=0 "

rc=0; ./rs_bin localhost > o5 2> e5 || rc=$?
if [ "${rc}" -eq 1 ] && grep -a -q "CAP001: capability 'net'" e5 && [ "$(cat o5)" = "start" ]; then
    expect "5 no net grant: exit 1, CAP001 'net', nothing after the call" "ok" "ok"
else
    expect "5 no net grant" "exit ${rc}, stdout $(tr '\n' '|' < o5), stderr $(head -c 160 e5)" "exit 1, CAP001 'net', stdout start"
fi

echo ""
echo "C1864: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
