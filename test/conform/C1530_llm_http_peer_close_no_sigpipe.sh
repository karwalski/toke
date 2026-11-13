#!/usr/bin/env bash
# C1530_llm_http_peer_close_no_sigpipe.sh — std.llm's plain http:// client
# writes its request with send(MSG_NOSIGNAL) on a socket that also has
# SO_NOSIGPIPE, so a local model server that has already closed is an error,
# never SIGPIPE (story 127.388).
#
# THE DEFECT.  llm.c's tcp_connect() never set SO_NOSIGPIPE and conn_send()
# wrote plain-HTTP bytes with send(fd, ..., 0), so an http:// model server
# (a local inference server on 127.0.0.1) that closed before reading the
# request killed the toke program with status 141.  127.351 (C1491) covered
# only the https:// path.  The fix: conn_send passes TK_SEND_FLAGS, and
# tcp_connect calls tk_sock_nosigpipe(fd) before connect
# (src/stdlib/tk_nosigpipe.h).  llm_tool.c and the glue files open no
# sockets: every std.llm / std.llmtool request goes through http_exchange's
# conn_send, which is the only send in llm.c.
#
# WHY IT IS DETERMINISTIC (the C1491 pattern: socketpair, closed peer, no
# clock).  The probe links the real llm.c and defines its own connect(),
# which the static link binds llm.c's call to: it dup2()s one end of a fresh
# AF_UNIX socketpair over the TCP socket and closes the other end, so the
# request write meets a peer that is gone (EPIPE, every time).  SIGPIPE is at
# its default disposition in the probe.  The probe's socket replaces the one
# tcp_connect made SO_NOSIGPIPE (and Darwin refuses SO_NOSIGPIPE, EINVAL, on a
# socket whose peer has closed), so what it proves is conn_send's
# MSG_NOSIGNAL -- the guard that holds on every platform.
#   control — send(fd, ..., 0) on a closed socketpair is killed (rc 141), so
#             a probe that cannot observe SIGPIPE at all cannot pass.
#   chat    — llm_chat() over http:// is an error and the probe lives (two
#             attempts: max_retries 1).
#   stream  — llm_chatstream() over http:// (the second http_exchange
#             caller) is an error and the probe lives.
#
# Negative control: C1530_STDLIB=<dir holding a788172c's src/stdlib> — the
# chat and stream cases are killed (rc 141); see the commit.
#
# Story: 127.388

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SL="${C1530_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1530_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1

cat > probe.c <<'C'
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include "llm.h"
#include "capabilities.h"

static int nconnect;
/* Every connect() made by llm.c lands here: the model server is gone. */
int connect(int fd, const struct sockaddr *a, socklen_t l)
{
    int sv[2];
    (void)a; (void)l;
    nconnect++;
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return -1;
    if (dup2(sv[0], fd) < 0) return -1;
    close(sv[0]);
    close(sv[1]);
    return 0;
}

int main(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "control";
    setvbuf(stdout, NULL, _IONBF, 0);
    signal(SIGPIPE, SIG_DFL);
    tk_cap_init(argc, argv);
    if (strcmp(mode, "control") == 0) {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) return 2;
        close(sv[1]);
        ssize_t w = send(sv[0], "POST", 4, 0);
        printf("survived %zd\n", w);
        return 0;
    }
    TkLlmClient *c = llm_client("http://127.0.0.1:9/v1", "", "m");
    if (!c) return 7;
    c->max_retries = 1;
    TkLlmMsg m = { "user", "hi" };
    if (strcmp(mode, "chat") == 0) {
        TkLlmResp r = llm_chat(c, &m, 1, 0.0);
        printf("survived err=%d connects=%d\n", r.is_err, nconnect);
        return 0;
    }
    if (strcmp(mode, "stream") == 0) {
        TkLlmStream r = llm_chatstream(c, &m, 1, 0.0);
        printf("survived err=%d connects=%d\n", r.is_err, nconnect);
        return 0;
    }
    return 2;
}
C

CF=(-std=c11 -D_GNU_SOURCE -Wall -Wextra -Werror -iquote "${SL}")
# The link set comes from scripts/stdlib_link_set.sh as cached objects (C696, C703).
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${SL}" --tkc "${TKC:-${REPO_ROOT}/tkc}" --cc "${CC}" \
           --objects llm -- "${CF[@]}")" \
    || { echo "FAIL: stdlib link set"; echo "C1530: 0 passed, 1 failed"; exit 1; }
LS=(); while IFS= read -r l; do LS+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" "${CF[@]}" -o probe probe.c "${LS[@]}" -lpthread > build.log 2>&1; then
    echo "FAIL: the probe does not build"; head -8 build.log; echo "C1530: 0 passed, 1 failed"; exit 1
fi
run() { out="$(trap - PIPE; ./probe "$1" --allow-net 2>&1 | head -c 4000; exit "${PIPESTATUS[0]}")"; rc=$?; }

run control
expect "0-control-send-flags-0-on-a-closed-peer-is-killed-by-SIGPIPE" "rc=${rc}" "rc=141"
run chat
expect "1-llm_chat-http-request-to-a-closed-peer-is-err-not-SIGPIPE"  "rc=${rc} ${out}" "rc=0 survived err=1 connects=2"
run stream
expect "2-llm_chatstream-http-to-a-closed-peer-is-err-not-SIGPIPE"    "rc=${rc} ${out}" "rc=0 survived err=1 connects=1"

echo "C1530: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
