#!/usr/bin/env bash
# C670_file_read_fifo_bounded.sh -- file.read of a FIFO never hangs, and reads
# a FIFO's bytes to end-of-file (story 121.73).
#
# THE DEFECT.  file_read() opened with fopen() -- no O_NONBLOCK -- while the
# byte calls' file_open_for_read() passes it.  open(2) of a FIFO no process
# has open for writing blocks until a writer arrives, so file.read of such a
# path hung the program for good.  And with a writer present, fseek/ftell
# on the FIFO failed, so file.read could not read a FIFO at all ("seek
# failed").  Measured on main 437dc471: case 1 hung (killed at the bound),
# case 2 printed ERR.
#
# THE FIX.  file_read opens as file_open_for_read does (O_RDONLY |
# O_NOFOLLOW | O_NONBLOCK) and reads a FIFO with the 127.254 non-blocking
# read + 100 ms select() loop; a writerless FIFO reads as end-of-file, "".
#
# WHAT IS ASSERTED (every run bounded; a hang is a FAIL, never a stall)
#   1. a writerless FIFO: file.read returns $ok, promptly;
#   2. a FIFO with a writer that closes after 0.5 s: file.read returns exactly
#      the bytes written;
#   3. a regular file, a directory ($err), a missing path ($err) and a
#      symlink ($err, AMB-07) behave as before.
#
# Negative control: C670_STDLIB=<a src/stdlib tree> builds the probe against
# it (via TKC_STDLIB_DIR).  With main's tree cases 1 and 2 fail.
#
# Story: 121.73
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C670_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c670_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C670: file.read of a FIFO is bounded and reads to end-of-file (121.73)"
echo "----------------------------------------------------------------------"

cat > rd.tk <<'EOF'
m=rd;
i=file:std.file;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let p=mt args.get(1){$ok:v v;$err:e ""};
  let r=mt file.read(p) {$ok:v v;$err:e "ERR"};
  io.print(r);
  <0
};
EOF
if ! TKC_STDLIB_DIR="${S}" "${TKC}" rd.tk --out rd >rd.cl 2>&1; then
    echo "  FAIL: compile rd.tk"; sed 's/^/      | /' rd.cl | head -5; exit 1
fi

# run_bounded SECS OUT CMD...: stdout to OUT; 124 if CMD overran and was killed.
run_bounded() {
    local secs="$1" out="$2"; shift 2
    "$@" > "${out}" 2>/dev/null & local p=$! t=0
    while kill -0 "${p}" 2>/dev/null; do
        if [ "${t}" -ge $((secs * 10)) ]; then
            kill -9 "${p}" 2>/dev/null; wait "${p}" 2>/dev/null; return 124
        fi
        sleep 0.1; t=$((t + 1))
    done
    wait "${p}"
}

# ── 1: writerless FIFO ────────────────────────────────────────────────────
mkfifo empty.fifo
rc=0; run_bounded 20 o1 ./rd --allow-read empty.fifo || rc=$?
if [ "${rc}" -eq 0 ] && [ ! -s o1 ]; then
    pass "1 writerless FIFO: \$ok \"\", no hang"
else
    fail "1 writerless FIFO: rc=${rc} (124 = hung, killed at 20 s) out=$(head -c 40 o1)"
fi

# ── 2: FIFO with a writer that closes later ───────────────────────────────
mkfifo data.fifo
cat > feed.py <<'EOF'
import os, subprocess, sys, time
fifo, prog = sys.argv[1], sys.argv[2]
fd = os.open(fifo, os.O_RDWR)          # a writer is open before the reader
os.write(fd, b"through-the-fifo\n")
p = subprocess.Popen([prog, "--allow-read", fifo], stdout=subprocess.PIPE)
time.sleep(0.5)
os.close(fd)                           # the writer's close is the end-of-file
try:
    out, _ = p.communicate(timeout=20)
except subprocess.TimeoutExpired:
    p.kill(); p.wait(); print("TIMEOUT"); sys.exit(0)
sys.stdout.write("rc=%d out=%r" % (p.returncode, out))
EOF
out2="$(python3 feed.py "${WORK}/data.fifo" "${WORK}/rd")"
if [ "${out2}" = "rc=0 out=b'through-the-fifo\\n'" ]; then
    pass "2 FIFO with a writer: read the bytes written"
else
    fail "2 FIFO with a writer: ${out2}"
fi

# ── 3: unchanged cases ────────────────────────────────────────────────────
printf 'regular\n' > reg.txt; mkdir adir; ln -s reg.txt link.txt
for c in "reg.txt|regular" "adir|ERR" "nope|ERR" "link.txt|ERR"; do
    p="${c%%|*}"; want="${c#*|}"
    rc=0; run_bounded 20 o3 ./rd --allow-read "${p}" || rc=$?
    got="$(tr -d '\n' < o3)"
    if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
        pass "3 ${p}: ${want}"
    else
        fail "3 ${p}: rc=${rc} got '${got}', want '${want}'"
    fi
done

echo "----------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
