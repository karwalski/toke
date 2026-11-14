#!/usr/bin/env bash
# C1611_log_returns_written.sh -- log.info/warn/error/debug return true
# exactly when the line was written (story 127.406).
#
# stdlib/log.tki declares log.info/warn/error -> bool and docs/stdlib/log.md
# says each returns true if the line was written, false if it was suppressed
# by the level threshold.  log_glue.c discarded tk_log_*'s result and returned
# 0, so every call read as false -- even when the line was printed.  The glue
# now returns that result, and log.c's write_line reports a short write or a
# failed flush as not written.
#
# WHAT IS ASSERTED (toke, run under TK_STR_VERIFY=1); the program encodes the
# four results in its exit code:
#   1. JSON, stderr open, default level info: log.info true, log.debug false
#      (filtered); after log.setlevel("ERROR"): log.warn false (filtered),
#      log.error true -- exit 9, and exactly the info and error lines written.
#   2. the same in text format (log.setformat("text")) -- exit 9.
#   3. stderr closed (2>&-): nothing can be written, so every call is false
#      -- exit 0.
#   4. log.debug under TK_LOG_LEVEL=DEBUG is true when written.
#
# Negative control: C1611_STDLIB=<dir> uses another tree's src/stdlib.  Run
# against the base (fix/z183-log-fields-rotation, 60823920): 1 passed,
# 3 failed -- every call returns false; only the closed-stderr case passes.
# With only the glue fixed (base log.c, unchecked fwrite): 3 passed, 1 failed
# -- closed stderr exits 9, the lost lines reported as written.
# Measured: this branch 4 passed.
#
# Story: 127.406
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1611_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1611_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1611: log.info & co. return true exactly when written (127.406)"
echo "--------------------------------------------------------------------------------"

# Exit code: 9 = info true, debug false, warn false, error true.
# 12/13 = a filtered call returned true; 14 = error false; 15 = info false.
gen() {
cat <<TOKE
m=$1;
i=log:std.log;
f=main():i64{
  $2
  let a=log.info("i"; @());
  let b=log.debug("d"; @());
  log.setlevel("ERROR");
  let c=log.warn("w"; @());
  let d=log.error("e"; @());
  if(b){<12};
  if(c){<13};
  if(a){
    if(d){<9};
    <14
  };
  if(d){<15};
  <0
};
TOKE
}
gen j "" > j.tk
gen t 'log.setformat("text");' > t.tk
cat > d.tk <<'TOKE'
m=d;
i=log:std.log;
f=main():i64{
  let a=log.debug("d"; @());
  if(a){<1};
  <0
};
TOKE

for p in j t d; do
    if ! "${TKC}" --allow-all --out "${p}" "${p}.tk" > "${p}.log" 2>&1; then
        fail "${p}.tk did not compile"; sed 's/^/        /' "${p}.log" | head -6
    fi
done

if [ -x j ]; then
    env -u TK_LOG_LEVEL TK_STR_VERIFY=1 ./j > j.out 2> j.err; rc=$?
    n="$(grep -c '^{' j.err)"
    if [ "${rc}" -eq 9 ] && [ "${n}" -eq 2 ] && grep -q '"msg":"i"' j.err && grep -q '"msg":"e"' j.err; then
        pass "json: info true, debug/warn false (filtered), error true; 2 lines written"
    else
        fail "json: exit ${rc} (want 9), ${n} lines (want 2)"
    fi
    # 127.439: not through env(1).  Ubuntu 26.04's env is uutils (Rust), and
    # the Rust runtime reopens a closed fd 0-2 on /dev/null at startup, so the
    # program inherited a WRITABLE stderr and every line was "written".  A
    # subshell that execs the program hands it the closed descriptor itself.
    ( unset TK_LOG_LEVEL; TK_STR_VERIFY=1; export TK_STR_VERIFY; exec ./j > j.out 2>&- ); rc=$?
    if [ "${rc}" -eq 0 ]; then
        pass "stderr closed: every call returns false"
    else
        fail "stderr closed: exit ${rc} (want 0: nothing was written)"
    fi
fi
if [ -x t ]; then
    env -u TK_LOG_LEVEL TK_STR_VERIFY=1 ./t > t.out 2> t.err; rc=$?
    if [ "${rc}" -eq 9 ] && [ "$(cat t.err)" = "$(printf '[INFO] msg=i\n[ERROR] msg=e')" ]; then
        pass "text: info true, debug/warn false, error true"
    else
        fail "text: exit ${rc} (want 9): '$(head -c 120 t.err)'"
    fi
fi
if [ -x d ]; then
    env TK_LOG_LEVEL=DEBUG TK_STR_VERIFY=1 ./d > d.out 2> d.err; rc=$?
    if [ "${rc}" -eq 1 ] && grep -q '"msg":"d"' d.err; then
        pass "log.debug under TK_LOG_LEVEL=DEBUG returns true"
    else
        fail "log.debug under TK_LOG_LEVEL=DEBUG: exit ${rc} (want 1)"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1611: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
