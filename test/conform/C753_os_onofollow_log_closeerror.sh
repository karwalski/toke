#!/usr/bin/env bash
# C753_os_onofollow_log_closeerror.sh — two C-only stdlib entry points gain a
# toke surface: os.onofollow() and log.closeerror() (story 136.77).
#
# THE GAPS.  tk_os_o_nofollow existed in os.c with no .tki export, header or
# doc, so os.open could not refuse a symlinked final path component (the
# AMB-07 rule every std.file open keeps).  tk_error_log_close existed in log.c
# and nothing in toke reached it: a program that opened an error log with
# log.openerror could never close it, and reopening it leaked a descriptor
# pair (the log and its .lock sidecar) every time.
#
# WHAT IS ASSERTED
#   1. os.onofollow() is the host's O_NOFOLLOW (python3 os.O_NOFOLLOW);
#   2. os.open(<regular file>; ordonly+onofollow; 0) succeeds (fd >= 0);
#   3. os.open(<symlink to it>; ordonly+onofollow; 0) is -1 and os.errno() is
#      ELOOP (python3 errno.ELOOP) -- and so is plain ordonly: os.open adds
#      O_NOFOLLOW to every open (AMB-07), as docs/stdlib/os.md says;
#   4. log.closeerror() with no error log open is false;
#   5. after log.openerror, log.closeerror() is true, and a second is false;
#   6. the close releases the descriptors: 300 openerror+closeerror cycles
#      under `ulimit -n 64` all open (0 failures).  The same loop without
#      closeerror fails once descriptors run out -- asserted as the in-test
#      negative control (failures > 0), so case 6 is not passing vacuously;
#   7. log.closeerror needs no grant (the program runs with --allow-write only
#      for the open).
#
# Story: 136.77
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${C753_STDLIB:-${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c753_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
printf 'x\n' > f.txt; ln -s f.txt l.txt; mkdir logs

cat > o.tk <<'EOF'
m=main;i=io:std.io;i=os:std.os;i=s:std.str;
f=main():i64{
  io.println(s.fromint(os.onofollow()));
  let fl = os.ordonly() + os.onofollow();
  let a = os.open("f.txt"; fl; 0);
  if(a>=0){io.println("regular ok")} el {io.println("regular failed")};
  let b = os.open("l.txt"; fl; 0);
  io.println(s.concat(s.fromint(b); s.concat(" "; s.fromint(os.errno()))));
  let c = os.open("l.txt"; os.ordonly(); 0);
  io.println(s.concat(s.fromint(c); s.concat(" "; s.fromint(os.errno()))));
  <0
};
EOF
cat > g.tk <<'EOF'
m=main;i=io:std.io;i=log:std.log;i=s:std.str;i=args:std.args;
f=yn(b:bool):$str{ if(b){<"true"}; <"false" };
f=main():i64{
  let mode = mt args.get(1) {$ok:v v;$err:e ""};
  if(s.eq(mode; "basic")){
    io.println(yn(log.closeerror()));
    log.openerror("logs/error.log"; 0; 0; 0);
    io.println(yn(log.closeerror()));
    io.println(yn(log.closeerror()));
    <0
  };
  let fails = mut.0;
  let i = mut.0;
  lp(i<300){
    if(log.openerror("logs/error.log"; 0; 0; 0) < 0){ fails = fails + 1 };
    if(s.eq(mode; "close")){ let c = log.closeerror() };
    i = i + 1
  };
  io.println(s.fromint(fails));
  <0
};
EOF
for p in o g; do
    if ! "${TKC}" ${p}.tk --out ${p} > ${p}.log 2>&1; then
        echo "FAIL build ${p}"; head -c 600 ${p}.log; echo; echo "C753: 0 passed, 1 failed"; exit 1
    fi
done
OUT="$(./o --allow-read 2>&1)"
line() { printf '%s\n' "${OUT}" | sed -n "$1p"; }
expect "1-onofollow-value"   "$(line 1)" "$(python3 -c 'import os; print(os.O_NOFOLLOW)')"
expect "2-regular-opens"     "$(line 2)" "regular ok"
expect "3-symlink-refused"   "$(line 3)" "-1 $(python3 -c 'import errno; print(errno.ELOOP)')"
expect "3b-plain-also-refused" "$(line 4)" "-1 $(python3 -c 'import errno; print(errno.ELOOP)')"
OUT="$(./g --allow-write basic 2>&1)"
expect "4-close-none"        "$(line 1)" "false"
expect "5-close-open"        "$(line 2)" "true"
expect "5b-close-twice"      "$(line 3)" "false"
expect "6-cycles-release-fds" "$( (ulimit -n 64; ./g --allow-write close) 2>&1)" "0"
KEEP="$( (ulimit -n 64; ./g --allow-write keep) 2>/dev/null)"
if [ -n "${KEEP}" ] && [ "${KEEP}" -gt 0 ] 2>/dev/null; then
    echo "PASS 6n-control-without-close-exhausts (${KEEP} of 300 failed)"; PASS=$((PASS + 1))
else
    echo "FAIL 6n-control-without-close-exhausts (got '${KEEP}')"; FAIL=$((FAIL + 1))
fi
echo "C753: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
