#!/usr/bin/env bash
# C633_os_write_all_or_error.sh — os.write(fd; data):i64!oserr writes every
# byte, retries EINTR, and reports a failure as an error value, not as a
# zero-byte write (121.70 (3)).
#
# THE DEFECT.  tk_os_write made ONE write(2) and returned `n < 0 ? 0 : n`:
#   - a write interrupted by a signal after part of the buffer moved returned
#     the part, and the rest was silently dropped (measured below: 77824 of
#     4194304 bytes reached a slow pipe reader under a 0.5 ms SIGALRM);
#   - EINTR before any byte moved returned 0 instead of retrying;
#   - a failure (EBADF, EPIPE, ENOSPC, ...) returned 0 -- exactly what a
#     successful write of "" returns -- so an error could not be told apart.
# The loop is now os_write_all() in os.c; the toke wrapper tk_os_write_w (in
# os_glue.c, because os.c is also linked into tkc, which has no tk_runtime)
# raises an `oserr {msg; code}` through the error slot.
#
# BEHAVIOUR CHANGE (interface): os.write returned `i64`; it returns
# `i64!oserr`.  `let n=o.write(fd;s)` still compiles (n is the byte count);
# a `mt` on it now sees failures.
#
# Cases:
#   1 C driver: os_write_all of 4 MiB into a pipe whose reader sleeps between
#     4 KiB reads, while SIGALRM (installed WITHOUT SA_RESTART) fires every
#     0.5 ms -> the reader receives all 4194304 bytes, err 0
#   2 the same run saw at least one signal (else the case proves nothing)
#   3 o.write(999;"x")  -> $err, e.code 9 (EBADF), the message names the
#     call, the errno text and the byte count
#   4 in that $err arm os.errno() == e.code
#   5 o.write(fd;"")    -> $ok 0 (a zero-byte write is a success)
#   6 o.write(fd;"abc") -> $ok 3, and the file holds "abc"
#   7 `let n=o.write(fd;"xy")` (no match) compiles and n is 2
#
# Negative control: C633_STDLIB=<main's src/stdlib at 2aa48c88, stdlib/ beside
# it> — case 1 compiles the driver against main's single-write tk_os_write:
# 77824 of 4194304 bytes (measured Darwin 25.6); cases 3-6 do not compile
# (os.write is plain i64 there, the $err arm has no payload to bind).
#
# Story: 121.70 (3)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
SL="${C633_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
export TKC_STDLIB_DIR="${SL}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT

# ── 1, 2: the loop, in C, under signals ───────────────────────────────────
cat > "${WORK}/drv.c" <<'EOF'
#define _GNU_SOURCE
#include "os.h"
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
static volatile sig_atomic_t hits = 0;
static void on_alrm(int s) { (void)s; hits++; }
int main(void) {
    size_t len = 4u << 20;
    char *buf = malloc(len + 1);
    if (!buf) return 2;
    for (size_t i = 0; i < len; i++) buf[i] = (char)('a' + i % 26);
    buf[len] = 0;
    int p[2]; if (pipe(p)) return 2;
    pid_t c = fork();
    if (c == 0) {                      /* slow reader: counts what arrives */
        close(p[1]);
        char r[4096]; long tot = 0; ssize_t n;
        struct timespec ts = {0, 200000};
        while ((n = read(p[0], r, sizeof r)) > 0) { tot += n; nanosleep(&ts, NULL); }
        printf("%ld\n", tot); fflush(stdout); _exit(0);
    }
    close(p[0]);
    struct sigaction sa; memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_alrm; sa.sa_flags = 0;   /* no SA_RESTART: EINTR / short writes */
    sigaction(SIGALRM, &sa, NULL);
    struct itimerval it = {{0, 500}, {0, 500}};
    setitimer(ITIMER_REAL, &it, NULL);
#ifdef MAIN_ABI
    long w = (long)tk_os_write((int64_t)p[1], (int64_t)(intptr_t)buf); int err = 0;
#else
    OsWriteResult r = os_write_all(p[1], buf, len); long w = (long)r.written; int err = r.err;
#endif
    struct itimerval off = {{0, 0}, {0, 0}};
    setitimer(ITIMER_REAL, &off, NULL);
    close(p[1]);
    int st; waitpid(c, &st, 0);
    fprintf(stderr, "wrote=%ld err=%d signals=%d\n", w, err, (int)hits);
    return 0;
}
EOF
ABI=""; grep -q 'os_write_all' "${SL}/os.h" || ABI="-DMAIN_ABI"
# 127.439: -D_GNU_SOURCE as the Makefile builds os.c (O_NOFOLLOW, which glibc
# declares only with it; macOS always does).
if cc -std=c99 -D_GNU_SOURCE ${ABI} -I"${SL}" "${WORK}/drv.c" "${SL}/os.c" "${SL}/capabilities.c" \
      -o "${WORK}/drv" > "${WORK}/drv.log" 2>&1; then
    GOT="$("${WORK}/drv" 2> "${WORK}/drv.err")"
    expect "1-every-byte-under-signals" "${GOT}|$(sed 's/ signals=.*//' "${WORK}/drv.err")" "4194304|wrote=4194304 err=0"
    SIGS="$(sed -n 's/.*signals=\([0-9]*\).*/\1/p' "${WORK}/drv.err")"
    expect "2-signals-delivered" "$([ "${SIGS:-0}" -gt 0 ] && echo yes || echo "no (${SIGS:-?})")" "yes"
else
    echo "FAIL 1 driver build"; head -5 "${WORK}/drv.log"; FAIL=$((FAIL + 2))
fi

# ── 3-7: the toke surface ─────────────────────────────────────────────────
cat > "${WORK}/p.tk" <<'EOF'
m=main;i=io:std.io;i=o:std.os;i=s:std.str;
f=main():i64{
  io.println(mt o.write(999;"x") {$ok:n s.concat("ok ";s.fromint(n));$err:e s.concat(s.fromint(e.code);s.concat("|";e.msg))});
  io.println(mt o.write(999;"x") {$ok:n "ok";$err:e s.fromint(o.errno()-e.code)});
  let fd=o.open("out.txt";o.owronly()+o.ocreat()+o.otrunc();420);
  io.println(mt o.write(fd;"") {$ok:n s.concat("ok ";s.fromint(n));$err:e e.msg});
  io.println(mt o.write(fd;"abc") {$ok:n s.concat("ok ";s.fromint(n));$err:e e.msg});
  let n=mt o.write(fd;"xy"){$ok:v v;$err:e 0};
  io.println(s.fromint(n));
  o.close(fd);
  <0
};
EOF
if ! "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/build.log" 2>&1; then
    echo "FAIL build"; head -c 600 "${WORK}/build.log"; echo; FAIL=$((FAIL + 5))
else
    OUT="$(cd "${WORK}" && ./b --allow-all 2>&1)"
    line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
    expect "3-badfd-err"          "$(line 1)" "9|os.write: Bad file descriptor: wrote 0 of 1 bytes"
    expect "4-errno-agrees"       "$(line 2)" "0"
    expect "5-empty-is-ok-0"      "$(line 3)" "ok 0"
    expect "6-write-ok-3"         "$(line 4)|$(cat "${WORK}/out.txt" 2>/dev/null)" "ok 3|abcxy"
    expect "7-unmatched-let"      "$(line 5)" "2"
fi
echo "C633: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
