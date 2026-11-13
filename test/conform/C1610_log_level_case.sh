#!/usr/bin/env bash
# C1610_log_level_case.sh -- log.setlevel and TK_LOG_LEVEL match level names
# case-insensitively (story 127.407).
#
# docs/stdlib/log.md says log.setlevel accepts "debug", "info", "warn" or
# "error" (case-insensitive), and its examples use lower case.  log.c compared
# against the upper-case names only, so log.setlevel("error") and
# TK_LOG_LEVEL=error were silently ignored and info lines kept printing.  It
# now matches ASCII case-insensitively (locale-independent).
#
# WHAT IS ASSERTED (toke, run under TK_STR_VERIFY=1), as the exact sequence of
# "msg" values written to stderr:
#   1. log.setlevel: "error" filters info/warn and keeps error; "Debug" lets
#      debug through; an unknown name ("verbose") keeps the previous level;
#      upper-case "WARN" still works.
#   2. TK_LOG_LEVEL: error, warn, Debug, INFO, an unknown value (-> info) and
#      unset (-> info) each filter debug/info/warn/error as documented.
# (Which value log.setlevel returns is NOT asserted: docs say bool, the .tki
# says i64 -- owner decision, story 127.410.)
#
# Negative control: C1610_STDLIB=<dir> uses another tree's src/stdlib.  Run
# against the base (fix/z183-log-fields-rotation, 60823920): 3 passed,
# 4 failed -- every lower/mixed-case case is ignored; only TK_LOG_LEVEL=INFO,
# the unknown value and unset pass.
# Measured: this branch 7 passed.
#
# Story: 127.407
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1610_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1610_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1610: log level names are case-insensitive (127.407)"
echo "--------------------------------------------------------------------------------"

# msgs <stderr file> -> the msg values, space-separated
msgs() { grep -o '"msg":"[^"]*"' "$1" | sed 's/^"msg":"//; s/"$//' | tr '\n' ' ' | sed 's/ $//'; }

cat > s.tk <<'TOKE'
m=s;
i=log:std.log;
f=main():i64{
  log.setlevel("error");
  log.info("i1"; @());
  log.warn("w1"; @());
  log.error("e1"; @());
  log.setlevel("Debug");
  log.debug("d1"; @());
  log.setlevel("verbose");
  log.debug("d2"; @());
  log.setlevel("WARN");
  log.info("i2"; @());
  log.warn("w2"; @());
  <0
};
TOKE
cat > e.tk <<'TOKE'
m=e;
i=log:std.log;
f=main():i64{
  log.debug("d"; @());
  log.info("i"; @());
  log.warn("w"; @());
  log.error("e"; @());
  <0
};
TOKE

if ! "${TKC}" --allow-all --out s s.tk > s.log 2>&1; then
    fail "s.tk did not compile"; sed 's/^/        /' s.log | head -6
else
    env -u TK_LOG_LEVEL TK_STR_VERIFY=1 ./s > s.out 2> s.err; rc=$?
    got="$(msgs s.err)"
    if [ "${rc}" -eq 0 ] && [ "${got}" = "e1 d1 d2 w2" ]; then
        pass "log.setlevel: \"error\" filters, \"Debug\" admits debug, unknown keeps it, \"WARN\" works"
    else
        fail "log.setlevel sequence (rc=${rc}): got '${got}' want 'e1 d1 d2 w2'"
    fi
fi

if ! "${TKC}" --allow-all --out e e.tk > e.log 2>&1; then
    fail "e.tk did not compile"; sed 's/^/        /' e.log | head -6
else
    for spec in "error|e" "warn|w e" "Debug|d i w e" "INFO|i w e" "loud|i w e" "-|i w e"; do
        lv="${spec%%|*}"; want="${spec#*|}"
        if [ "${lv}" = "-" ]; then
            env -u TK_LOG_LEVEL TK_STR_VERIFY=1 ./e > e.out 2> e.err; rc=$?; lab="unset"
        else
            env TK_LOG_LEVEL="${lv}" TK_STR_VERIFY=1 ./e > e.out 2> e.err; rc=$?; lab="${lv}"
        fi
        got="$(msgs e.err)"
        if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
            pass "TK_LOG_LEVEL=${lab}: writes '${want}'"
        else
            fail "TK_LOG_LEVEL=${lab} (rc=${rc}): got '${got}' want '${want}'"
        fi
    done
fi

echo "--------------------------------------------------------------------------------"
echo "C1610: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
