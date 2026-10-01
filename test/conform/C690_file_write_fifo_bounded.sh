#!/usr/bin/env bash
# C690_file_write_fifo_bounded.sh -- file.write, file.append and file.copy
# into a FIFO nobody reads return a clear $err at once instead of blocking
# forever, and still deliver every byte when a reader is there (story 121.77).
#
# THE DEFECT.  file.c's fopen_nofollow opened with O_NOFOLLOW but without
# O_NONBLOCK.  open(2) of a FIFO for writing blocks until a reader opens it,
# so file.write / file.append / file.copy with a readerless FIFO as the
# destination hung the program (same class as 121.73 for file.read).
#
# THE FIX.  fopen_nofollow opens O_NONBLOCK -- a readerless FIFO fails with
# ENXIO, reported as $notregular "a FIFO with no reader ..." -- then clears
# O_NONBLOCK before stdio writes, so a slow reader is waited for rather than
# a short write failing with EAGAIN.
#
# WHAT IS ASSERTED (every run bounded at 20 s; 124 = hung and killed)
#   1-3. write / append / copy into a readerless FIFO: prompt $notregular;
#   4.   copy FROM a writerless FIFO: prompt $ok, an empty copy;
#   5-7. write / append / copy into a FIFO with a reader: $ok, and the reader
#        receives exactly the bytes (copy: 200,000 bytes, larger than the
#        FIFO buffer, so the blocking-write path is exercised);
#   8.   write / append to a regular file unchanged.
#
# Negative control: C690_STDLIB=<a src/stdlib tree> builds the probes against
# it (via TKC_STDLIB_DIR).  With main's tree cases 1-3 hang (124).
#
# Story: 121.77
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C690_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c690_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C690: file.write/append/copy on a FIFO are bounded (121.77)"
echo "------------------------------------------------------------"

ARMS='$isdir:m "isdir";$symlink:m "symlink";$io:m "io";$notfound:m "notfound";$notregular:m "notregular";$permission:m "permission";$invalid:m "invalid";$toolarge:m "toolarge";$nomem:m "nomem";$badarg:m "badarg"'
for op in write append; do
cat > "${op}.tk" <<EOF
m=${op};
i=file:std.file;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let r=mt file.${op}(args.get(1);"hello\n") {\$ok:v "ok";\$err:e mt e {${ARMS}}};
  io.print(r);
  <0
};
EOF
done
cat > copy.tk <<EOF
m=copy;
i=file:std.file;
i=io:std.io;
i=args:std.args;
f=main():i64{
  let r=mt file.copy(args.get(1);args.get(2)) {\$ok:v "ok";\$err:e mt e {${ARMS}}};
  io.print(r);
  <0
};
EOF
for p in write append copy; do
    if ! TKC_STDLIB_DIR="${S}" "${TKC}" "${p}.tk" --out "${p}" >"${p}.cl" 2>&1; then
        echo "  FAIL: compile ${p}.tk"; sed 's/^/      | /' "${p}.cl" | head -5; exit 1
    fi
done

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

mkfifo ff
printf 'source\n' > src.txt
# ── 1-3: readerless FIFO as the destination ───────────────────────────────
n=1
for c in "write|ff" "append|ff" "copy|src.txt ff"; do
    prog="${c%%|*}"; args="${c#*|}"
    rc=0; run_bounded 20 o ./"${prog}" --allow-read --allow-write ${args} || rc=$?
    got="$(cat o)"
    if [ "${rc}" -eq 0 ] && [ "${got}" = "notregular" ]; then
        pass "${n} ${prog} into a readerless FIFO: prompt \$notregular"
    else
        fail "${n} ${prog} into a readerless FIFO: rc=${rc} (124 = hung, killed at 20 s) got '${got}'"
    fi
    n=$((n + 1))
done

# ── 4: writerless FIFO as the copy source ─────────────────────────────────
rm -f dst.txt
rc=0; run_bounded 20 o ./copy --allow-read --allow-write ff dst.txt || rc=$?
if [ "${rc}" -eq 0 ] && [ "$(cat o)" = "ok" ] && [ -f dst.txt ] && [ ! -s dst.txt ]; then
    pass "4 copy from a writerless FIFO: prompt \$ok, empty copy"
else
    fail "4 copy from a writerless FIFO: rc=${rc} got '$(cat o)'"
fi

# ── 5-7: a FIFO with a reader receives every byte ─────────────────────────
cat > reader.py <<'EOF'
import os, select, subprocess, sys, time
fifo, prog = sys.argv[1], sys.argv[2:]
big = open("big.txt", "rb").read()
fd = os.open(fifo, os.O_RDONLY | os.O_NONBLOCK)   # the reader is open first
p = subprocess.Popen(prog, stdout=subprocess.PIPE)
data, seen, t0 = b"", False, time.time()
while time.time() - t0 < 20:
    select.select([fd], [], [], 0.1)
    try:
        c = os.read(fd, 65536)
    except BlockingIOError:
        continue
    if c:
        data += c; seen = True; continue
    if seen or p.poll() is not None:
        break
os.close(fd)
try:
    out, _ = p.communicate(timeout=20)
except subprocess.TimeoutExpired:
    p.kill(); p.wait(); print("TIMEOUT"); sys.exit(0)
want = big if "big.txt" in prog else b"hello\n"
print("%s %d %s" % (out.decode(), len(data), "same" if data == want else "differ"))
EOF
python3 -c 'open("big.txt","wb").write(bytes((i * 7) % 251 for i in range(200000)))'
n=5
for c in "write ff|ok 6 same" "append ff|ok 6 same" "copy big.txt ff|ok 200000 same"; do
    cmd="${c%%|*}"; want="${c#*|}"
    set -- ${cmd}; prog="$1"; shift
    got="$(python3 reader.py ff ./"${prog}" --allow-read --allow-write "$@")"
    if [ "${got}" = "${want}" ]; then
        pass "${n} ${prog} into a FIFO with a reader: ${want}"
    else
        fail "${n} ${prog} into a FIFO with a reader: got '${got}', want '${want}'"
    fi
    n=$((n + 1))
done

# ── 8: regular files unchanged ────────────────────────────────────────────
rm -f reg.txt
r1="$(./write --allow-write reg.txt)"; r2="$(./append --allow-write reg.txt)"
if [ "${r1}${r2}" = "okok" ] && [ "$(cat reg.txt)" = "$(printf 'hello\nhello')" ]; then
    pass "8 regular file: write then append"
else
    fail "8 regular file: '${r1}' '${r2}' content '$(cat reg.txt)'"
fi

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
