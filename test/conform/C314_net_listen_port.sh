#!/usr/bin/env bash
# C314_net_listen_port.sh — net.listen never dereferences a port number
# (story 138.13, loke T-49).
#
# THE DEFECT.  net.listen reads "host:port" as a C string, and its interface
# was the untyped (i64) -> i64 [abi], so `net.listen(11444)` compiled and the
# glue's strrchr() dereferenced address 11444: SIGSEGV, rc 139.
#
# THE FIX.  stdlib/net.tki declares the parameter `str`, and since 138.3 (c)
# (Batch GA) the checker rejects an integer there (E4031 at --check).  As a
# defence the glue still takes a value below 65536 -- never a valid pointer
# on a supported platform -- as a PORT and binds it to 127.0.0.1 only.
#
# WHAT IS ASSERTED (C driver under ASan+UBSan, then a toke program):
#   - tk_net_listen_w(<port>) returns a listening fd bound to 127.0.0.1:<port>
#     (getsockname), not to every interface
#   - tk_net_listen_w("127.0.0.1:<port>") still works; "nonsense", 0 and a
#     negative value return 0
#   - T-49's program shape is E4031 at --check, naming net.listen's str
#     parameter; with the address as a str it runs and prints a positive fd
#
# Negative control: C314_STDLIB=<dir>.  Measured 2026-09-27 against main's
# net_glue.c (c15dd11): the driver dies with SIGSEGV on the first case.
#
# Story: 138.13
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C314_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list that a stdlib
# dependency change silently breaks.
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# 127.270: the stdlib set is linked as cached objects built with these flags.
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --cc "${CC}" --objects net -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set: see stderr above"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c314_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C314: net.listen with a port number (138.13 T-49)"
echo "---------------------------------------------------"
# Two free loopback ports, chosen by the kernel.
read -r P1 P2 <<<"$(python3 -c '
import socket
s=[socket.socket() for _ in range(2)]
[x.bind(("127.0.0.1",0)) for x in s]
print(*[x.getsockname()[1] for x in s]); [x.close() for x in s]')"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "capabilities.h"
int64_t tk_net_listen_w(int64_t addr);
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
static int bound_to(int fd, const char *ip, int port) {
    struct sockaddr_in sa; socklen_t l = sizeof sa; char b[64];
    if (getsockname(fd, (struct sockaddr *)&sa, &l) != 0) return 0;
    inet_ntop(AF_INET, &sa.sin_addr, b, sizeof b);
    return !strcmp(b, ip) && ntohs(sa.sin_port) == port;
}
int main(int argc, char **argv) {
    (void)argc;
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    int p1 = atoi(argv[1]), p2 = atoi(argv[2]);
    int64_t fd = tk_net_listen_w((int64_t)p1);
    CHECK(fd > 0, "listen(<port number>) returns a listening fd (no crash)");
    CHECK(fd > 0 && bound_to((int)fd, "127.0.0.1", p1), "... bound to 127.0.0.1:<port>, not every interface");
    char a[64]; snprintf(a, sizeof a, "127.0.0.1:%d", p2);
    int64_t fd2 = tk_net_listen_w((int64_t)(intptr_t)a);
    CHECK(fd2 > 0 && bound_to((int)fd2, "127.0.0.1", p2), "listen(\"127.0.0.1:<port>\") still works");
    CHECK(tk_net_listen_w((int64_t)(intptr_t)"nonsense") == 0, "listen(\"nonsense\") is 0");
    CHECK(tk_net_listen_w(0) == 0 && tk_net_listen_w(-5) == 0, "listen(0) and listen(-5) are 0");
    if (fd > 0) close((int)fd);
    if (fd2 > 0) close((int)fd2);
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
if ! "${CC}" "${CF[@]}" -o t t.c \
        "${LINK[@]}" > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t "${P1}" "${P2}" > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing:"
        grep -a 'ERROR: AddressSanitizer\|SEGV\|runtime error' t.out | head -3 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

cat > t49.tk <<TOKE
m=t49;i=io:std.io;i=net:std.net;
f=main():i64{
  let fd=net.listen(${P1});
  io.println("listen returned \(fd)");
  <0
};
TOKE
TKC_STDLIB_DIR="${S}" "${TKC}" --check t49.tk > t49.log 2>&1; rc=$?
if [ "${rc}" -ne 0 ] && grep -a -q '"error_code":"E4031".*argument 1 of .net.listen. is declared .str.' t49.log; then
    echo "  PASS: T-49's program: net.listen(<i64>) is E4031 at --check"; PASS=$((PASS + 1))
else
    echo "  FAIL: T-49's program (rc=${rc}): expected E4031 on net.listen's str parameter"
    sed 's/^/        /' t49.log | head -4; FAIL=$((FAIL + 1))
fi
cat > t49s.tk <<TOKE
m=t49s;i=io:std.io;i=net:std.net;
f=main():i64{
  let fd=net.listen("127.0.0.1:${P1}");
  io.println("listen returned \(fd)");
  <0
};
TOKE
if TKC_STDLIB_DIR="${S}" "${TKC}" --out t49s t49s.tk > t49s.log 2>&1; then
    got="$(./t49s --allow-net 2>&1)"; rc=$?
    if [ "${rc}" -eq 0 ] && printf '%s' "${got}" | grep -a -q '^listen returned [1-9]'; then
        echo "  PASS: net.listen(\"127.0.0.1:<port>\"): rc 0, ${got}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: net.listen(str): rc=${rc} out='${got}'"; FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: t49s.tk did not compile"; sed 's/^/        /' t49s.log | head -4; FAIL=$((FAIL + 1))
fi

echo "---------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
