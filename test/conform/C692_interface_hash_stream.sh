#!/usr/bin/env bash
# C692_interface_hash_stream.sh -- src/ir.c interface_hash reads the .tki with
# tk_read_stream (readall.h), not an fgetc loop, and does not hang on a FIFO
# (story 121.77 item 2; residue of 127.263).
#
# THE DEFECT.  interface_hash fopen()ed the path and hashed it with an fgetc
# loop: open(2) of a FIFO with no writer blocks forever, and a blocking stdio
# read of a FIFO can miss the writer's close on Darwin under load (127.254).
#
# THE FIX.  Opened O_NONBLOCK (then switched back), read whole with
# tk_read_stream -- the 127.254 non-blocking FIFO loop -- and hashed from the
# buffer.  The hash of every byte sequence is unchanged.
#
# interface_hash has no caller in the driver today, so this compiles src/ir.c
# (with src/diag.c, which it links against) into a small harness.
#
# WHAT IS ASSERTED (every run bounded at 20 s; 124 = hung and killed)
#   1. the djb2-xor hash of "abc\n" is 2087551791, and a missing path is 0;
#   2. a 300,000-byte file holding every byte value hashes to the value an
#      independent Python djb2-xor gives (high bytes taken unsigned);
#   3. a writerless FIFO returns promptly (the hash of nothing, 5381);
#   4. a FIFO fed the same bytes as case 2 by a writer hashes the same.
#
# Negative control: C692_SRC=<a src tree>.  With main's tree case 3 hangs.
#
# Story: 121.77
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
SRC="${C692_SRC:-${REPO_ROOT}/src}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c692_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C692: interface_hash reads with tk_read_stream (121.77)"
echo "-------------------------------------------------------"

cat > ih.c <<'EOF'
#include <stdio.h>
#include <stdint.h>
uint32_t interface_hash(const char *path);
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++) printf("%u\n", (unsigned)interface_hash(argv[i]));
    return 0;
}
EOF
if ! "${CC}" -std=c99 -D_GNU_SOURCE -I"${SRC}" ih.c "${SRC}/ir.c" "${SRC}/diag.c" -o ih >ih.cl 2>&1; then
    echo "  FAIL: compile harness"; sed 's/^/      | /' ih.cl | head -5; exit 1
fi

run_bounded() {   # run_bounded SECS OUT CMD...: stdout to OUT; 124 if killed
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

printf 'abc\n' > a.tki
got="$(./ih a.tki missing.tki | tr '\n' ' ')"
[ "${got}" = "2087551791 0 " ] && pass "1 known hash, missing path 0" || fail "1 got '${got}'"

python3 - <<'EOF'
data = bytes((i * 131 + i // 256) % 256 for i in range(300000))
open("big.tki", "wb").write(data)
h = 5381
for c in data:
    h = (((h << 5) + h) ^ c) & 0xffffffff
open("big.want", "w").write("%d\n" % h)
EOF
want="$(cat big.want)"
got="$(./ih big.tki)"
[ "${got}" = "${want}" ] && pass "2 300,000 bytes: ${got}" || fail "2 got '${got}', want '${want}'"

mkfifo empty.fifo
rc=0; run_bounded 20 o3 ./ih empty.fifo || rc=$?
if [ "${rc}" -eq 0 ] && [ "$(cat o3)" = "5381" ]; then
    pass "3 writerless FIFO: prompt, 5381"
else
    fail "3 writerless FIFO: rc=${rc} (124 = hung, killed at 20 s) out '$(cat o3)'"
fi

mkfifo data.fifo
cat > feed.py <<'EOF'
import os, subprocess, sys, time
fifo, prog = sys.argv[1], sys.argv[2]
fd = os.open(fifo, os.O_RDWR)            # a writer is open before the reader
p = subprocess.Popen([prog, fifo], stdout=subprocess.PIPE)
time.sleep(0.3)
os.set_blocking(fd, True)
data = open("big.tki", "rb").read()
off = 0
while off < len(data):
    off += os.write(fd, data[off:off + 8192])
os.close(fd)                             # the writer's close is end-of-file
try:
    out, _ = p.communicate(timeout=20)
except subprocess.TimeoutExpired:
    p.kill(); p.wait(); print("TIMEOUT"); sys.exit(0)
print(out.decode().strip())
EOF
got="$(python3 feed.py "${WORK}/data.fifo" "${WORK}/ih")"
[ "${got}" = "${want}" ] && pass "4 FIFO with a writer hashes like the file" || fail "4 got '${got}', want '${want}'"

echo "-------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
