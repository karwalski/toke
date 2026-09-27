#!/usr/bin/env bash
# C392_bind_sites_no_shadow.sh — the non-http bind sites cannot shadow a live
# listener, and a port in TIME_WAIT still binds (story 135.33).
#
# THE DEFECT.  router.serve (router.c), net.listen (net_glue.c),
# net.portavailable (net.c) and proc_config_test (server_ops.c) set
# SO_REUSEADDR before every bind.  On Darwin that lets 127.0.0.1:P bind
# beside a live *:P listener (and *:P beside a live 127.0.0.1:P); the more
# specific socket then takes that address's traffic.  So router.serve and
# net.listen started on a busy port and took loopback traffic, and the two
# "is this port free?" checks answered yes for a port a server held.  135.32
# fixed the same thing in http.c with bind_exclusive(); this moves it to
# src/stdlib/tk_bind.h and uses it at the other sites.
#
# WHAT IS ASSERTED (a C driver linked against the stdlib sources; the live
# listener A is a plain socket in the driver, with SO_REUSEADDR set):
#   1. A on *:P.  net.listen("127.0.0.1:P") is 0; after it, a connection to
#      127.0.0.1:P reaches A; net.portavailable(P) is 0; proc_config_test
#      on 127.0.0.1:P fails; router.serve(r;"127.0.0.1";P) returns $err
#      (a child process -- if the bind succeeded it would serve forever,
#      and is reported as "started")
#   2. A on 127.0.0.1:P.  The wildcard forms: net.listen("0.0.0.0:P") is 0
#      and A keeps loopback; proc_config_test with no host fails;
#      router.serve(r;"";P) returns $err
#   3. P in TIME_WAIT on the server side (asserted: a plain bind without
#      SO_REUSEADDR is EADDRINUSE and nothing listens, so the case cannot
#      pass vacuously).  net.listen binds, net.portavailable is 1,
#      proc_config_test passes, router.serve starts
#   4. a toke program: router.serve(r;"127.0.0.1";P) beside a live *:P
#      takes the $err arm (exit 7) instead of serving
#
# Negative control: C392_STDLIB=<main's src/stdlib> -- the case-1 and case-2
# assertions fail on Darwin (each site binds and shadows A), case 3 passes.
# On Linux cases 1 and 2 pass either way: that is the point.
#
# Known gap, asserted nowhere because it is not fixed: a wildcard bind
# probes loopback only (tk_bind.h), so a listener on one non-loopback
# address alone is not detected.
#
# Story: 135.33
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C392_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0; SRV=""
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c392_XXXXXX")"
cleanup() { [ -n "${SRV}" ] && kill -9 "${SRV}" 2>/dev/null; rm -rf "${WORK}"; }
trap cleanup EXIT
cd "${WORK}"
echo "C392: router/net/server_ops binds cannot shadow a live listener (135.33)"
echo "------------------------------------------------------------------------"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "capabilities.h"
#include "net.h"
#include "router.h"
#include "server_ops.c"      /* proc_config_test and ProcConfig are file-local */

int64_t tk_net_listen_w(int64_t addr);
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)

static struct sockaddr_in sin_of(const char *ip, int port) {
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port);
    inet_pton(AF_INET, ip, &a.sin_addr);
    return a;
}

/* A live listener on ip:port (port 0 = any); its port in *port. */
static int listener(const char *ip, int *port) {
    int s = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = sin_of(ip, *port);
    if (bind(s, (struct sockaddr *)&a, sizeof a) != 0 || listen(s, 16) != 0) {
        perror("listener"); exit(2);
    }
    socklen_t l = sizeof a; getsockname(s, (struct sockaddr *)&a, &l);
    *port = ntohs(a.sin_port);
    return s;
}

/* 1 when a connection to 127.0.0.1:port is queued on listening fd A. */
static int reaches(int A, int port) {
    int c = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a = sin_of("127.0.0.1", port);
    if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) { close(c); return 0; }
    struct pollfd p = { A, POLLIN, 0 };
    int ok = poll(&p, 1, 1000) == 1;
    if (ok) { int x = accept(A, NULL, NULL); if (x >= 0) close(x); }
    close(c);
    return ok;
}

/* router.serve in a child: "err" when it returned $err, "started" when it was
 * still serving after 1.5 s, "ok" when it returned without an error. */
static const char *router_try(const char *host, int port) {
    pid_t pid = fork();
    if (pid == 0) {
        TkRouter *r = router_new();
        TkRouterErr e = router_serve(r, host, (uint64_t)port);
        _exit(e.failed ? 10 : 11);
    }
    for (int i = 0; i < 15; i++) {
        int st;
        if (waitpid(pid, &st, WNOHANG) == pid)
            return WIFEXITED(st) && WEXITSTATUS(st) == 10 ? "err" : "ok";
        usleep(100000);
    }
    kill(pid, SIGKILL); waitpid(pid, NULL, 0);
    return "started";
}

static int cfg_test(const char *host, int port) {
    ProcConfig c; memset(&c, 0, sizeof c);
    c.host = host; c.port = (uint64_t)port; c.workers = 1;
    return proc_config_test(&c);
}

int main(void) {
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    char addr[64];
    int P;

    /* 1. A on *:P, specific-address binds */
    P = 0; int A = listener("0.0.0.0", &P);
    snprintf(addr, sizeof addr, "127.0.0.1:%d", P);
    int64_t fd = tk_net_listen_w((int64_t)(intptr_t)addr);
    CHECK(fd == 0, "1a net.listen(\"127.0.0.1:P\") beside a live *:P is 0");
    CHECK(reaches(A, P), "1b ... and 127.0.0.1:P still reaches A");
    if (fd > 0) close((int)fd);
    CHECK(net_portavailable((uint64_t)P) == 0, "1c net.portavailable(P) is false while *:P listens");
    CHECK(cfg_test("127.0.0.1", P) == -1, "1d proc_config_test(127.0.0.1:P) fails while *:P listens");
    const char *rr = router_try("127.0.0.1", P);
    printf("      router.serve -> %s\n", rr);
    CHECK(!strcmp(rr, "err"), "1e router.serve(r;\"127.0.0.1\";P) beside a live *:P is $err");
    CHECK(reaches(A, P), "1f ... and 127.0.0.1:P still reaches A");
    close(A);

    /* 2. A on 127.0.0.1:P, wildcard binds */
    P = 0; A = listener("127.0.0.1", &P);
    snprintf(addr, sizeof addr, "0.0.0.0:%d", P);
    fd = tk_net_listen_w((int64_t)(intptr_t)addr);
    CHECK(fd == 0, "2a net.listen(\"0.0.0.0:P\") beside a live 127.0.0.1:P is 0");
    CHECK(reaches(A, P), "2b ... and 127.0.0.1:P still reaches A");
    if (fd > 0) close((int)fd);
    CHECK(cfg_test(NULL, P) == -1, "2c proc_config_test(*:P) fails while 127.0.0.1:P listens");
    rr = router_try("", P);
    printf("      router.serve -> %s\n", rr);
    CHECK(!strcmp(rr, "err"), "2d router.serve(r;\"\";P) beside a live 127.0.0.1:P is $err");
    CHECK(reaches(A, P), "2e ... and 127.0.0.1:P still reaches A");
    close(A);

    /* 3. server-side TIME_WAIT on 127.0.0.1:P: the server closes first */
    P = 0; A = listener("127.0.0.1", &P);
    {
        int c = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = sin_of("127.0.0.1", P);
        if (connect(c, (struct sockaddr *)&a, sizeof a) != 0) { perror("connect"); return 2; }
        int x = accept(A, NULL, NULL);
        close(x);            /* server side sends FIN first -> its TIME_WAIT */
        usleep(100000);
        close(c);
        close(A);
        usleep(200000);
    }
    {
        int s = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in a = sin_of("127.0.0.1", P);
        int rc = bind(s, (struct sockaddr *)&a, sizeof a), e = errno;
        close(s);
        CHECK(rc != 0 && e == EADDRINUSE,
              "3a precondition: a plain bind of 127.0.0.1:P is EADDRINUSE (TIME_WAIT)");
        int c = socket(AF_INET, SOCK_STREAM, 0);
        CHECK(connect(c, (struct sockaddr *)&a, sizeof a) != 0,
              "3b precondition: nothing listens on 127.0.0.1:P");
        close(c);
    }
    snprintf(addr, sizeof addr, "127.0.0.1:%d", P);
    fd = tk_net_listen_w((int64_t)(intptr_t)addr);
    CHECK(fd > 0, "3c net.listen(\"127.0.0.1:P\") over TIME_WAIT binds");
    if (fd > 0) close((int)fd);
    CHECK(net_portavailable((uint64_t)P) == 1, "3d net.portavailable(P) over TIME_WAIT is true");
    CHECK(cfg_test("127.0.0.1", P) == 0, "3e proc_config_test(127.0.0.1:P) over TIME_WAIT passes");
    rr = router_try("127.0.0.1", P);
    printf("      router.serve -> %s\n", rr);
    CHECK(!strcmp(rr, "started"), "3f router.serve(r;\"127.0.0.1\";P) over TIME_WAIT starts");

    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
if ! "${CC}" -std=c11 -D_GNU_SOURCE -D_DARWIN_C_SOURCE -g -O1 -iquote "${S}" -o t t.c \
        "${S}/net_glue.c" "${S}/net.c" "${S}/router.c" "${S}/ws.c" "${S}/capabilities.c" "${S}/metrics.c" \
        -lz > cc.log 2>&1; then
    echo "  FAIL: driver build: $(head -c 800 cc.log)"; FAIL=$((FAIL + 1))
else
    ./t > t.out 2>&1
    rc=$?
    grep -a '^  PASS\|^  FAIL\|^      router' t.out
    PASS=$((PASS + $(grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
    if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
        echo "  FAIL: the driver died (rc=${rc}) before finishing:"; tail -3 t.out | sed 's/^/        /'
        FAIL=$((FAIL + 1))
    fi
fi

# 4. the toke surface: router.serve beside a live wildcard listener
read -r P A_PID < <(python3 -c '
import socket,os,sys,time
s=socket.socket(); s.setsockopt(socket.SOL_SOCKET,socket.SO_REUSEADDR,1)
s.bind(("0.0.0.0",0)); s.listen(8)
pid=os.fork()
if pid==0:
    time.sleep(30); os._exit(0)
print(s.getsockname()[1], pid); sys.stdout.flush(); os._exit(0)
' 2>/dev/null)
SRV="${A_PID:-}"
case4() {
    if [ -z "${P:-}" ]; then
        echo "  FAIL: 4 could not start the wildcard listener"; FAIL=$((FAIL + 1)); return
    fi
    cat > rs.tk <<TOKE
m=rs;
i=router:std.router;
f=h(req:i64):i64{ <router.ok("B") };
f=main():i64{
  let r=router.new();
  router.get(r;"/";&h);
  <mt router.serve(r;"127.0.0.1";${P}) {\$ok:v 0; \$err:e 7}
};
TOKE
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" --allow-all --out rs_bin rs.tk > rs.build 2>&1; then
        echo "  FAIL: 4 rs.tk does not build"; sed 's/^/        /' rs.build | head -4; FAIL=$((FAIL + 1)); return
    fi
    ./rs_bin > rs.out 2> rs.err &
    local B=$! rc=0
    for _ in $(seq 1 20); do kill -0 "${B}" 2>/dev/null || break; sleep 0.1; done
    if kill -0 "${B}" 2>/dev/null; then
        kill -9 "${B}" 2>/dev/null; wait "${B}" 2>/dev/null
        echo "  FAIL: 4 toke router.serve(r;\"127.0.0.1\";P) beside a live *:P started serving"; FAIL=$((FAIL + 1)); return
    fi
    wait "${B}" || rc=$?
    if [ "${rc}" -eq 7 ]; then
        echo "  PASS: 4 toke router.serve(r;\"127.0.0.1\";P) beside a live *:P takes \$err (exit 7)"; PASS=$((PASS + 1))
    else
        echo "  FAIL: 4 expected exit 7, got ${rc}"; sed 's/^/        | /' rs.err | head -3; FAIL=$((FAIL + 1))
    fi
}
case4

echo "------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
