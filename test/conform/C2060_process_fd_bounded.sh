#!/usr/bin/env bash
# C2060_process_fd_bounded.sh — a long-running spawner does not run out of
# file descriptors: std.process closes a child's stdin pipe once the child
# has been reaped (story 138.48).
#
# THE DEFECT.  process.spawn opens three pipes.  stdout/stderr close at EOF,
# but the write end of the child's stdin was closed only by
# process_handle_free, which no toke call reaches (no call frees a handle:
# wait/stdout/stderr answer from the record afterwards, 138.36).  So every
# spawn kept one descriptor for the life of the program: 400 iterations of
# spawn /usr/bin/true + wait + stdout + stderr under `ulimit -n 256` gave
# ok=246 bad=154 -- from the 247th spawn on, process.spawn failed.
#
# THE FIX (row option (a), no API change).  process.c closes stdin_fd when it
# reaps the child (wait, a poll/hasexited/isalive that sees the exit, the
# timeout path).  A write to an exited child is meaningless; the record keeps
# the exit code and the drained output, so wait/stdout/stderr answer as before.
#
# WHAT IS ASSERTED.
#   1. Under `ulimit -n 64`, 300 spawn+wait+stdout+stderr iterations all
#      succeed (ok=300 bad=0), and so do 300 spawn+wait without reading the
#      output, and 300 spawn + poll-until-exited (the reap through poll).
#   2. A reaped handle still answers: wait twice gives the same code, stdout
#      after wait gives the child's output, kill after wait is false.
#
# Negative control: TKC=<base ee27591c tree>/toke (measured 2026-11-23) --
# full ok=54 bad=246, waitonly ok=54 bad=246, pollreap ok=18 bad=282; the
# reaped case passes on both.
#
# Story: 138.48
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2060_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2060: std.process closes a reaped child's stdin pipe (138.48)"
echo "----------------------------------------------------------------------"

run_case() {   # name expected-output program-body
    local name="$1" want="$2"
    cat > "${name}.tk"
    if ! "${TKC}" -o "${name}" "${name}.tk" > "${name}.log" 2>&1; then
        fail "${name}: build ($(head -c 200 "${name}.log"))"; return
    fi
    local got
    got="$(bash -c "ulimit -n 64; ./${name} --allow-all" 2>&1)"
    if [ "${got}" = "${want}" ]; then pass "${name}: ${got}"
    else fail "${name}: want '${want}', got '${got}'"; fi
}

run_case full "ok=300 bad=0" <<'EOF'
m=full;
i=io:std.io;
i=process:std.process;
f=main():i64{
  let ok=mut.0;
  let bad=mut.0;
  lp(let n=0;n<300;n=n+1){
    let h=mt process.spawn(@("/usr/bin/true")) {$ok:v v;$err:e 0};
    let w=process.wait(h);
    let o=mt process.stdout(h) {$ok:s s;$err:e "E"};
    let r=mt process.stderr(h) {$ok:s s;$err:e "E"};
    if(w==0 && o=="" && r==""){ok=ok+1;}el{bad=bad+1;};
  };
  io.println("ok=\(ok) bad=\(bad)");
  <0
};
EOF

run_case waitonly "ok=300 bad=0" <<'EOF'
m=waitonly;
i=io:std.io;
i=process:std.process;
f=main():i64{
  let ok=mut.0;
  let bad=mut.0;
  lp(let n=0;n<300;n=n+1){
    let h=mt process.spawn(@("/usr/bin/true")) {$ok:v v;$err:e 0};
    if(process.wait(h)==0){ok=ok+1;}el{bad=bad+1;};
  };
  io.println("ok=\(ok) bad=\(bad)");
  <0
};
EOF

run_case pollreap "ok=300 bad=0" <<'EOF'
m=pollreap;
i=io:std.io;
i=process:std.process;
f=main():i64{
  let ok=mut.0;
  let bad=mut.0;
  lp(let n=0;n<300;n=n+1){
    let h=mt process.spawn(@("/usr/bin/true")) {$ok:v v;$err:e 0};
    let spins=mut.0;
    lp(let k=0;process.poll(h)==false && k<100000;k=k+1){process.sleep(1);spins=spins+1;};
    if(process.poll(h) && process.exitcode(h)==0){ok=ok+1;}el{bad=bad+1;};
  };
  io.println("ok=\(ok) bad=\(bad)");
  <0
};
EOF

run_case reaped "w=3 again=3 out=[hi] kill=false" <<'EOF'
m=reaped;
i=io:std.io;
i=process:std.process;
f=main():i64{
  let h=mt process.spawn(@("/bin/sh";"-c";"echo hi; exit 3")) {$ok:v v;$err:e 0};
  let w=process.wait(h);
  let w2=process.wait(h);
  let o=mt process.stdout(h) {$ok:s s;$err:e "E"};
  let k=process.kill(h);
  io.println("w=\(w) again=\(w2) out=[\(o.trim())] kill=\(k)");
  <0
};
EOF

echo "----------------------------------------------------------------------"
echo "C2060: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
