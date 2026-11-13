#!/usr/bin/env bash
# C699_file_write_close_errors.sh -- file.write, file.append and file.copy
# report a write that fails at flush/close as $err (never $ok), survive a
# FIFO reader that closes early (no SIGPIPE death), leave the program's
# SIGPIPE disposition and signal mask as they were, and file.copy names the
# path that failed (story 121.84).
#
# THE DEFECTS.
#   (1) file_write / file_append checked fputs only.  stdio buffers, so the
#       bytes reach the file in fflush/fclose -- whose results were ignored:
#       ENOSPC or EPIPE there was reported as $ok.  A FIFO whose reader closed
#       early also raised SIGPIPE, whose default action kills the program
#       (exit 141) before it can see any result.  file_copy had the same
#       SIGPIPE exposure.
#   (2) tk_file_copy_w named src in every error, even when dst failed.
#
# THE FIX.  file.c checks fputs, fflush and fclose; EPIPE/ENOSPC/EDQUOT are
# $io with a message naming the cause; the write runs with SIGPIPE blocked in
# the calling thread's mask only, and a SIGPIPE the write raised is consumed
# with sigwait before the mask is restored.  The process-wide disposition is
# never changed.  file_copy_at reports which side failed; the glue names it.
#
# WHAT IS ASSERTED (every run bounded at 20 s; 124 = hung and killed)
#   1-3. write / append / copy of 1 MB into a FIFO whose reader takes one
#        byte and closes: the program exits 0 (not 141) and prints $io with
#        "EPIPE" in the message; the copy's message names the FIFO (dst).
#   4.   copy from a missing src names src; 5. copy into a missing directory
#        names dst, not src.
#   6.   C probe (threads): after an EPIPE write, SIGPIPE's disposition is
#        still SIG_DFL, the thread's mask does not hold SIGPIPE, and no
#        SIGPIPE is left pending; a SIGPIPE pending BEFORE the write is
#        still pending after it (not swallowed).  The reader thread, which
#        has SIGPIPE unblocked, stays alive until the write returns
#        (127.273): Darwin sends the SIGPIPE to the process, so that thread
#        used to take it -- rc 141 or a pass depending on whether it had
#        already exited.  Now deterministic; fixed by F_SETNOSIGPIPE.
#   7.   Linux only: write / append / copy to /dev/full -> $io "ENOSPC".
#        Skipped on other systems (no portable full device; a RAM disk needs
#        privileges and is not portable).
#   8.   regular files unchanged.
#
# Negative control: C699_STDLIB=<a src/stdlib tree, with its stdlib/ .tki
# tree at ../../stdlib> builds the probes against it.  With main's tree
# (2026-10-01) cases 1-3 and 6 die of SIGPIPE (rc -13 from python, 141 from
# the shell) and case 5 names src; 4 and 8 pass.
# With main's file.c of 2026-10-27 (8bb9f63e, before F_SETNOSIGPIPE) case 6
# fails every run on Darwin (rc 141); the rest pass (127.273).
#
# Stories: 121.84, 127.273
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C699_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c699_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C699: file.write/append/copy check flush+close, survive EPIPE (121.84)"
echo "------------------------------------------------------------"

ARMS='$isdir:m s.concat("isdir|";m);$symlink:m s.concat("symlink|";m);$io:m s.concat("io|";m);$notfound:m s.concat("notfound|";m);$notregular:m s.concat("notregular|";m);$permission:m s.concat("permission|";m);$invalid:m s.concat("invalid|";m);$toolarge:m s.concat("toolarge|";m);$nomem:m s.concat("nomem|";m);$badarg:m s.concat("badarg|";m)'
# write/append: args (dst; file whose content is written)
for op in write append; do
cat > "${op}.tk" <<EOF
m=${op};
i=file:std.file;
i=io:std.io;
i=s:std.str;
i=args:std.args;
f=main():i64{
  let c=mt file.read(args.get(2)) {\$ok:t t;\$err:e ""};
  let r=mt file.${op}(args.get(1);c) {\$ok:v "ok";\$err:e mt e {${ARMS}}};
  io.println(r);
  <0
};
EOF
done
cat > copy.tk <<EOF
m=copy;
i=file:std.file;
i=io:std.io;
i=s:std.str;
i=args:std.args;
f=main():i64{
  let r=mt file.copy(args.get(1);args.get(2)) {\$ok:v "ok";\$err:e mt e {${ARMS}}};
  io.println(r);
  <0
};
EOF
for p in write append copy; do
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" "${p}.tk" --out "${p}" >"${p}.cl" 2>&1; then
        echo "  FAIL: compile ${p}.tk"; sed 's/^/      | /' "${p}.cl" | head -5; exit 1
    fi
done

python3 -c 'open("big.txt","wb").write(bytes(65 + (i % 26) for i in range(1048576)))'
mkfifo ff

# The reader opens the FIFO first (so the writer's non-blocking open finds
# it), starts the program, takes ONE byte -- proof the writer is writing, with
# ~1 MB still to go through a 64 KB pipe -- then closes.  The writer's next
# write(2) is EPIPE.  Prints "<rc>|<stdout>".
cat > early.py <<'EOF'
import os, select, subprocess, sys, time
fifo, prog = sys.argv[1], sys.argv[2:]
fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)
p = subprocess.Popen(prog, stdout=subprocess.PIPE)
t0 = time.time()
while time.time() - t0 < 20:
    r, _, _ = select.select([fd], [], [], 0.1)
    if r:
        try:
            if os.read(fd, 1): break
        except BlockingIOError:
            pass
    if p.poll() is not None: break
os.close(fd)
try:
    out, _ = p.communicate(timeout=20)
except subprocess.TimeoutExpired:
    p.kill(); p.wait(); print("124|TIMEOUT"); sys.exit(0)
print("%d|%s" % (p.returncode, out.decode().strip()))
EOF

# ── 1-3: FIFO reader closes early ─────────────────────────────────────────
n=1
for c in "write ff big.txt" "append ff big.txt" "copy big.txt ff"; do
    set -- ${c}; prog="$1"; shift
    got="$(python3 early.py ff ./"${prog}" --allow-read --allow-write "$@")"
    rc="${got%%|*}"; out="${got#*|}"
    ok=1
    [ "${rc}" = "0" ] || ok=0
    case "${out}" in io\|*EPIPE*) ;; *) ok=0 ;; esac
    if [ "${prog}" = "copy" ]; then
        case "${out}" in *": ff") ;; *) ok=0 ;; esac
    fi
    if [ "${ok}" = 1 ]; then
        pass "${n} ${prog} into a FIFO whose reader closes early: rc 0, ${out}"
    else
        fail "${n} ${prog} into a FIFO whose reader closes early: rc=${rc} (-13 = killed by SIGPIPE) out '${out}'"
    fi
    n=$((n + 1))
done

# ── 4-5: copy names the failing path ──────────────────────────────────────
printf 'x\n' > src.txt
out="$(./copy --allow-read --allow-write missing.txt dst.txt)"
case "${out}" in notfound\|*": missing.txt") pass "4 copy from a missing src names src: ${out}" ;;
    *) fail "4 copy from a missing src: '${out}'" ;; esac
out="$(./copy --allow-read --allow-write src.txt nodir/dst.txt)"
case "${out}" in notfound\|*": nodir/dst.txt") pass "5 copy into a missing directory names dst: ${out}" ;;
    *) fail "5 copy into a missing directory: '${out}' (want the dst path named)" ;; esac

# ── 6: signal state after an EPIPE write (C probe, threaded reader) ──────
cat > probe.c <<'EOF'
#include "file.h"
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <unistd.h>
static int rfd;
static int done[2];              /* main -> reader: "file_write has returned" */
static void *reader(void *a) {   /* wait for one byte, take it, close */
    (void)a; char c;
    for (int i = 0; i < 200; i++) {
        fd_set s; FD_ZERO(&s); FD_SET(rfd, &s);
        struct timeval tv = {0, 100000};
        if (select(rfd + 1, &s, NULL, NULL, &tv) > 0 && read(rfd, &c, 1) == 1) break;
    }
    close(rfd);
    /* 127.273: stay alive until the write has returned.  On Darwin the
     * SIGPIPE is process-directed, and this thread (SIGPIPE unblocked in
     * the first run) is where it lands -- the case used to pass or die
     * depending on whether this thread had already exited. */
    while (read(done[0], &c, 1) < 0 && errno == EINTR) {}
    return NULL;
}
static int pipe_pending(void) {
    sigset_t p; sigemptyset(&p); sigpending(&p); return sigismember(&p, SIGPIPE) == 1;
}
static int pipe_masked(void) {
    sigset_t m; pthread_sigmask(SIG_BLOCK, NULL, &m); return sigismember(&m, SIGPIPE) == 1;
}
static int run(const char *fifo, char *big, int pre_pending) {
    rfd = open(fifo, O_RDONLY | O_NONBLOCK);
    if (rfd < 0) { perror("open reader"); return 2; }
    if (pipe(done) != 0) { perror("pipe"); return 2; }
    pthread_t t; pthread_create(&t, NULL, reader, NULL);
    BoolFileResult r = file_write(fifo, big);
    (void)write(done[1], "x", 1);
    pthread_join(t, NULL);
    close(done[0]); close(done[1]);
    struct sigaction sa; sigaction(SIGPIPE, NULL, &sa);
    int bad = 0;
    if (!r.is_err || !r.err.msg || !strstr(r.err.msg, "EPIPE")) { printf("not-epipe:%s ", r.is_err ? r.err.msg : "ok"); bad = 1; }
    if (sa.sa_handler != SIG_DFL) { printf("disposition-changed "); bad = 1; }
    if (pre_pending) {
        if (!pipe_pending()) { printf("pre-pending-swallowed "); bad = 1; }
    } else {
        if (pipe_masked()) { printf("mask-left-blocked "); bad = 1; }
        if (pipe_pending()) { printf("left-pending "); bad = 1; }
    }
    return bad;
}
int main(int argc, char **argv) {
    (void)argc;
    size_t n = 1 << 20; char *big = malloc(n + 1);
    memset(big, 'z', n); big[n] = 0;
    int bad = run(argv[1], big, 0);
    /* a SIGPIPE pending before the call (blocked by the program itself)
     * must still be pending after it */
    sigset_t one; sigemptyset(&one); sigaddset(&one, SIGPIPE);
    pthread_sigmask(SIG_BLOCK, &one, NULL);
    raise(SIGPIPE);
    bad |= run(argv[1], big, 1);
    printf(bad ? "BAD\n" : "GOOD\n");
    return bad;
}
EOF
# 127.295: file.c reads with tk_strlen (str.c holds the sealed heap's state),
# so the probe takes std.file's link set from stdlib_link_set.sh --objects
# rather than compiling file.c alone.
PCF=(-std=c99 -D_GNU_SOURCE -I"${S}")
PLINK="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects file -- "${PCF[@]}" 2>probe.ls)" || PLINK=""
PLS=(); while IFS= read -r l; do [ -n "$l" ] && PLS+=("$l"); done <<< "${PLINK}"
if [ -z "${PLINK}" ]; then
    fail "6 probe: stdlib link set failed"; sed 's/^/      | /' probe.ls | head -5
elif "${CC}" "${PCF[@]}" -o probe probe.c "${PLS[@]}" -lz -lpthread -lm >probe.cl 2>&1; then
    rc=0; out="$(./probe ff 2>&1)" || rc=$?
    if [ "${rc}" -eq 0 ] && [ "${out}" = "GOOD" ]; then
        pass "6 signal state after EPIPE: disposition SIG_DFL, mask restored, nothing left pending, prior pending kept"
    else
        fail "6 signal state after EPIPE: rc=${rc} (141 = killed by SIGPIPE) '${out}'"
    fi
else
    fail "6 probe did not build"; sed 's/^/      | /' probe.cl | head -5
fi

# ── 7: ENOSPC via /dev/full (Linux only) ──────────────────────────────────
if [ "$(uname -s)" = "Linux" ] && [ -c /dev/full ]; then
    n=7
    for c in "write /dev/full src.txt" "append /dev/full src.txt" "copy src.txt /dev/full"; do
        set -- ${c}; prog="$1"; shift
        out="$(./"${prog}" --allow-read --allow-write "$@")"
        case "${out}" in io\|*ENOSPC*) pass "7 ${prog} to /dev/full: ${out}" ;;
            *) fail "7 ${prog} to /dev/full: '${out}' (want io|...ENOSPC...)" ;; esac
    done
else
    echo "  SKIP: 7 ENOSPC via /dev/full -- $(uname -s) has no /dev/full, and a full"
    echo "        filesystem cannot be made portably without privileges"
fi

# ── 8: regular files unchanged ────────────────────────────────────────────
printf 'hello\n' > h.txt
rm -f reg.txt
r1="$(./write --allow-read --allow-write reg.txt h.txt)"
r2="$(./append --allow-read --allow-write reg.txt h.txt)"
r3="$(./copy --allow-read --allow-write reg.txt reg2.txt)"
if [ "${r1}${r2}${r3}" = "okokok" ] && [ "$(cat reg2.txt)" = "$(printf 'hello\nhello')" ]; then
    pass "8 regular file: write, append, copy"
else
    fail "8 regular file: '${r1}' '${r2}' '${r3}' content '$(cat reg2.txt 2>/dev/null)'"
fi

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
