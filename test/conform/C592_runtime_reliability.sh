#!/usr/bin/env bash
# C592_runtime_reliability.sh -- std.process pipe handling, file.listall under
# threads, and the compiler arena's size arithmetic (story 121.29; audit-120
# reliability.md REL-03..REL-06).  C harnesses run under ASan+UBSan, each
# with a watchdog so a deadlock is a BAD line, not a hung suite.
#
# WHAT IS ASSERTED, and what main (f605aec9) did
#   process REL-03  wait() then stdout()/stderr() on a child printing 300 KB
#                   to each -- main: the child blocked on a full pipe while
#                   the parent sat in waitpid; both asleep forever
#           REL-03  stdin_write of 300 KB to `cat` before reading anything --
#                   main: cat filled stdout and stopped reading; deadlock
#           REL-03  stdout() while the child fills stderr -- main: hung
#           REL-03  timeout(5 s) on a child that prints 300 KB then exits --
#                   main: the child could not exit, so it was SIGKILLed
#           REL-04  spawn when FD_CLOEXEC cannot be set on the error pipe
#                   fails at once -- main: ignored the failure and spawn
#                   blocked for the child's whole life (`sleep 2`: ~2 s)
#   exec    REL-03  a toke program: process.exec("head -c 200000 big.txt")
#                   prints 200000; process.exec("cat") returns -- main: both
#                   hung (exec waited before reading, and kept stdin open)
#   listall REL-05  8 threads x 40 file_listall over 200 files: every call
#                   returns 200 entries, no ASan report -- main shared one
#                   static accumulator across threads
#   arena   REL-06  arena_alloc(-1) and arena_alloc(INT_MAX) return NULL --
#                   main: 1 usable byte for -1; signed overflow (UBSan) and a
#                   2 GiB memset past a 64 KiB block for INT_MAX
#
# Negative control: C592_STDLIB=<dir> (a copy of src/stdlib) and
# C592_ARENA=<file> (an arena.c) point the suite at other sources; main's
# process.c/process_glue.c/file.c/arena.c each fail their case.  Recorded in
# the 121.29 report.
#
# Story: 121.29
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C592_STDLIB:-${REPO_ROOT}/src/stdlib}"
# 127.268: the stdlib link sets come from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not hand lists that a stdlib
# dependency change silently breaks.  t_process.c #includes process.c itself
# (to wrap fcntl), so it is left out of the process set.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" --except process.c process)" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK_PROCESS=(); while IFS= read -r l; do LINK_PROCESS+=("$l"); done <<< "${LINKSET}"
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --tkc "${TKC}" --stdlib "${S}" file)" \
    || { echo "FAIL: stdlib link set: see stderr above"; exit 1; }
LINK_FILE=(); while IFS= read -r l; do LINK_FILE+=("$l"); done <<< "${LINKSET}"
ARENA_C="${C592_ARENA:-${REPO_ROOT}/src/arena.c}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c592_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"

echo "C592: runtime reliability -- process pipes, listall, arena (121.29)"
echo "------------------------------------------------------------------------"

case_run() {
    local name="$1" src="$2"; shift 2
    # shellcheck disable=SC2086
    if ! "${CC}" -std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}" -I"${REPO_ROOT}/src" \
            -o "${WORK}/${name}" "${src}" "$@" >"${WORK}/${name}.cc" 2>&1; then
        fail "${name}: build: $(grep -a -m3 -E 'error' "${WORK}/${name}.cc" | head -c 400)"
        return
    fi
    (cd "${WORK}" && "./${name}") >"${WORK}/${name}.out" 2>&1
    local rc=$?
    local ok bad
    ok=$(grep -a -c '^OK ' "${WORK}/${name}.out")
    bad=$(grep -a -c '^BAD ' "${WORK}/${name}.out")
    grep -a -E '^(OK|BAD) ' "${WORK}/${name}.out" | sed "s/^OK /  ok   ${name}: /; s/^BAD /  bad  ${name}: /"
    if [ "${rc}" -ne 0 ] && [ "${bad}" -eq 0 ]; then
        fail "${name}: exit ${rc} -- $(grep -a -m1 -E 'ERROR: AddressSanitizer|runtime error' "${WORK}/${name}.out" | head -c 300)"
    elif [ "${bad}" -ne 0 ] || [ "${ok}" -eq 0 ]; then
        fail "${name}: ${bad} check(s) failed"
    else
        pass "${name}: ${ok} check(s)"
    fi
}

WATCHDOG_C='
#include <signal.h>
#include <unistd.h>
static const char *g_watch = "startup";
static void on_alarm(int sig) {
    (void)sig;
    char b[256];
    int n = snprintf(b, sizeof b, "BAD watchdog: stuck in \"%s\"\n", g_watch);
    write(1, b, (size_t)n);
    _exit(1);
}
static void watch(const char *what, unsigned secs) { g_watch = what; signal(SIGALRM, on_alarm); alarm(secs); }
'

# ── process ────────────────────────────────────────────────────────────────
{
cat <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
/* REL-04 fault injection: fail F_SETFD while g_fail_setfd is set. */
static int g_fail_setfd = 0;
static int t_fcntl(int fd, int cmd, ...) {
    if (cmd == F_SETFD && g_fail_setfd) { errno = EBADF; return -1; }
    if (cmd == F_GETFD || cmd == F_GETFL) return fcntl(fd, cmd);
    va_list ap; va_start(ap, cmd); int arg = va_arg(ap, int); va_end(ap);
    return fcntl(fd, cmd, arg);
}
#define fcntl t_fcntl
C
echo "#include \"${S}/process.c\""
echo "#undef fcntl"
echo "${WATCHDOG_C}"
cat <<'C'
#define CHECK(c, m) do { printf("%s %s\n", (c) ? "OK" : "BAD", m); fflush(stdout); } while (0)
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec + t.tv_nsec / 1e9; }
static ProcessHandle *sp(const char *sh) {
    const char *cmd[] = { "/bin/sh", "-c", sh, NULL };
    SpawnResult r = process_spawn(cmd);
    return r.is_err ? NULL : r.ok;
}
int main(void) {
    ProcessHandle *h = sp("head -c 300000 /dev/zero; head -c 300000 /dev/zero >&2");
    watch("wait() on a child printing 300 KB to stdout and stderr", 20);
    WaitResult w = process_wait(h);
    StdoutResult o = process_stdout(h);
    StrProcessResult e = process_stderr(h);
    alarm(0);
    CHECK(!w.is_err && w.ok == 0, "REL-03: wait() returns while the child writes more than a pipe holds");
    CHECK(!o.is_err && !e.is_err, "REL-03: stdout()/stderr() after wait() succeed");
    StdoutResult o2 = process_stdout(h);
    CHECK(o2.ok && o2.ok[0] == 0, "stdout() a second time returns \"\"");
    process_handle_free(h);

    h = sp("cat");
    static char big[300000]; memset(big, 'x', sizeof big);
    watch("stdin_write of 300 KB to cat before reading stdout", 20);
    U64ProcessResult wr = process_stdin_write(h, big, sizeof big);
    close(h->stdin_fd); h->stdin_fd = -1;
    o = process_stdout(h);
    alarm(0);
    CHECK(!wr.is_err && wr.ok == sizeof big, "REL-03: stdin_write of 300 KB to cat completes");
    CHECK(!o.is_err && o.ok && strlen(o.ok) == sizeof big, "REL-03: cat's 300 KB echo is all returned");
    process_wait(h); process_handle_free(h);

    h = sp("head -c 300000 /dev/zero >&2; echo done");
    watch("stdout() while the child fills stderr", 20);
    o = process_stdout(h);
    alarm(0);
    CHECK(!o.is_err && o.ok && strcmp(o.ok, "done\n") == 0, "REL-03: stdout() returns while the child fills stderr");
    e = process_stderr(h);
    CHECK(!e.is_err && e.ok, "stderr drained meanwhile is still returned");
    process_wait(h); process_handle_free(h);

    h = sp("head -c 300000 /dev/zero | tr '\\0' y; exit 3");
    watch("timeout() on a child printing 300 KB", 20);
    I32ProcessResult t = process_timeout(h, 5000);
    alarm(0);
    CHECK(!t.is_err && t.ok == 3, "REL-03: timeout(5 s): the child exits on its own (code 3, not killed)");
    o = process_stdout(h);
    CHECK(o.ok && strlen(o.ok) == 300000, "REL-03: its output was kept");
    process_handle_free(h);

    g_fail_setfd = 1;
    const char *cmd[] = { "/bin/sleep", "2", NULL };
    double t0 = now();
    watch("spawn with FD_CLOEXEC failing", 10);
    SpawnResult sr = process_spawn(cmd);
    alarm(0);
    double dt = now() - t0;
    g_fail_setfd = 0;
    CHECK(sr.is_err && dt < 1.0, "REL-04: spawn fails at once when FD_CLOEXEC cannot be set");
    if (!sr.is_err) { printf("    (spawn returned a handle after %.2f s)\n", dt); process_wait(sr.ok); process_handle_free(sr.ok); }
    return 0;
}
C
} > "${WORK}/t_process.c"
case_run process "${WORK}/t_process.c" "${LINK_PROCESS[@]}"

# ── exec: through tkc ──────────────────────────────────────────────────────
if [ -x "${TKC}" ]; then
    cat > "${WORK}/p.tk" <<'EOF'
m=p;
i=process:std.process;
i=io:std.io;
i=str:std.str;
f=main():i64{
  let out=process.exec("head -c 200000 big.txt");
  io.println(str.fromint(str.len(out)));
  let c=process.exec("cat");
  io.println(str.concat("cat:";c));
  <0
};
EOF
    head -c 200000 /dev/zero | tr '\0' y > "${WORK}/big.txt"
    if (cd "${WORK}" && TKC_STDLIB_DIR="${S}" "${TKC}" p.tk --out p > p.cl 2>&1); then
        ( cd "${WORK}" && ./p --allow-run > p.out 2>&1 ) & ppid=$!
        for _ in $(seq 1 200); do kill -0 "${ppid}" 2>/dev/null || break; sleep 0.1; done
        if kill -0 "${ppid}" 2>/dev/null; then
            pkids="$(pgrep -P "${ppid}" 2>/dev/null)"
            for k in ${pkids}; do gk="$(pgrep -P "${k}" 2>/dev/null)"; kill ${gk} "${k}" 2>/dev/null; done
            kill "${ppid}" 2>/dev/null; wait "${ppid}" 2>/dev/null
            fail "exec: process.exec hung (20 s) -- $(head -c 100 "${WORK}/p.out")"
        else
            wait "${ppid}"
            if [ "$(sed -n 1p "${WORK}/p.out")" = 200000 ] && [ "$(sed -n 2p "${WORK}/p.out")" = "cat:" ]; then
                pass "exec: process.exec returns 200000 bytes, and \"cat\" sees EOF on stdin"
            else
                fail "exec: printed $(head -c 200 "${WORK}/p.out")"
            fi
        fi
    else
        fail "exec: probe does not compile: $(head -c 300 "${WORK}/p.cl")"
    fi
else
    fail "exec: tkc not found at ${TKC} (run make)"
fi

# ── listall ────────────────────────────────────────────────────────────────
mkdir -p "${WORK}/tree/a/b"
for i in $(seq 1 100); do : > "${WORK}/tree/f${i}"; : > "${WORK}/tree/a/b/g${i}"; done
{
cat <<C
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include "file.h"
C
echo "${WATCHDOG_C}"
cat <<'C'
static volatile int g_bad = 0;
static void *lister(void *a) {
    (void)a;
    for (int i = 0; i < 40; i++) {
        StrArrayFileResult r = file_listall("tree");
        if (r.is_err || r.ok.len != 200) g_bad++;
        for (uint64_t j = 0; j < r.ok.len; j++) { if (!r.ok.data[j]) g_bad++; free((void *)r.ok.data[j]); }
        free((void *)r.ok.data);
    }
    return NULL;
}
int main(void) {
    watch("8 threads listing", 60);
    pthread_t t[8];
    for (int i = 0; i < 8; i++) pthread_create(&t[i], NULL, lister, NULL);
    for (int i = 0; i < 8; i++) pthread_join(t[i], NULL);
    alarm(0);
    printf("%s REL-05: 8 threads x 40 file_listall: every call returned 200 entries (%d wrong)\n", g_bad ? "BAD" : "OK", g_bad);
    return 0;
}
C
} > "${WORK}/t_listall.c"
case_run listall "${WORK}/t_listall.c" "${LINK_FILE[@]}" -lpthread

# ── arena ──────────────────────────────────────────────────────────────────
cat > "${WORK}/t_arena.c" <<C
#include "${ARENA_C}"
C
cat >> "${WORK}/t_arena.c" <<'C'
#include <stdio.h>
#include <stdint.h>
#include <limits.h>
#define CHECK(c, m) do { printf("%s %s\n", (c) ? "OK" : "BAD", m); fflush(stdout); } while (0)
int main(void) {
    Arena *a = arena_init();
    CHECK(arena_alloc(a, -1) == NULL, "REL-06: negative size (an overflowed n*sizeof) returns NULL");
    CHECK(arena_alloc(a, INT_MIN) == NULL, "REL-06: INT_MIN returns NULL");
    CHECK(arena_alloc(a, INT_MAX) == NULL, "REL-06: INT_MAX (rounding would overflow) returns NULL");
    CHECK(arena_alloc(a, INT_MAX - 3) == NULL, "REL-06: INT_MAX-3 returns NULL");
    unsigned char *p = arena_alloc(a, 0);
    CHECK(p != NULL, "size 0 still returns a usable pointer");
    int ok = 1;
    for (int n = 1; n < 5000; n += 37) {
        unsigned char *q = arena_alloc(a, n);
        if (!q || ((uintptr_t)q % 8) != 0) { ok = 0; break; }
        for (int i = 0; i < n; i++) if (q[i]) { ok = 0; break; }
        for (int i = 0; i < n; i++) q[i] = 0xAB;
    }
    unsigned char *big = arena_alloc(a, 3 * 1024 * 1024);
    if (!big) ok = 0; else { big[0] = 1; big[3 * 1024 * 1024 - 1] = 1; }
    CHECK(ok, "ordinary allocations: aligned, zeroed, writable; a 3 MiB block too");
    arena_free(a);
    return 0;
}
C
case_run arena "${WORK}/t_arena.c"

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
