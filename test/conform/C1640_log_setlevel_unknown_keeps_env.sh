#!/usr/bin/env bash
# C1640_log_setlevel_unknown_keeps_env.sh -- an unrecognised log.setlevel name
# before the first log call keeps the TK_LOG_LEVEL level (story 127.411).
#
# docs/stdlib/log.md says an unrecognised log.setlevel name keeps the previous
# level.  Before the first log call that level is the one TK_LOG_LEVEL selects,
# but tk_log_set_level forced INFO when the level was still uninitialised, so
# log.setlevel("verbose") under TK_LOG_LEVEL=ERROR let info lines through.  It
# now initialises from the environment first.
#
# WHAT IS ASSERTED (toke, run under TK_STR_VERIFY=1), as the exact sequence of
# "msg" values written to stderr by a program that calls
# log.setlevel("verbose") and then logs debug/info/warn/error:
#   TK_LOG_LEVEL=ERROR -> "e"; =warn -> "w e"; =debug -> "d i w e";
#   unset -> "i w e"; =loud (unrecognised) -> "i w e".
# And a recognised name still overrides the environment: TK_LOG_LEVEL=ERROR
# then log.setlevel("debug") writes "d i w e".
# (Which value log.setlevel returns is NOT asserted -- owner decision 127.410.)
#
# Negative control: C1640_STDLIB=<dir> uses another tree's src/stdlib.  Run
# against the base (fix/z185-log-returns-level, f5c69dc6): 3 passed, 3 failed
# -- ERROR, warn and debug all come out as INFO ("i w e").
# Measured: this branch 6 passed.
#
# Story: 127.411
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1640_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1640_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1640: unknown log.setlevel name keeps TK_LOG_LEVEL (127.411)"
echo "--------------------------------------------------------------------------------"

msgs() { grep -o '"msg":"[^"]*"' "$1" | sed 's/^"msg":"//; s/"$//' | tr '\n' ' ' | sed 's/ $//'; }

for n in u k; do
    if [ "${n}" = u ]; then lv=verbose; else lv=debug; fi
    cat > "${n}.tk" <<TOKE
m=${n};
i=log:std.log;
f=main():i64{
  log.setlevel("${lv}");
  log.debug("d"; @());
  log.info("i"; @());
  log.warn("w"; @());
  log.error("e"; @());
  <0
};
TOKE
done

if ! "${TKC}" --allow-all --out u u.tk > u.log 2>&1; then
    fail "u.tk did not compile"; sed 's/^/        /' u.log | head -6
else
    for spec in "ERROR|e" "warn|w e" "debug|d i w e" "-|i w e" "loud|i w e"; do
        lv="${spec%%|*}"; want="${spec#*|}"
        if [ "${lv}" = "-" ]; then
            env -u TK_LOG_LEVEL TK_STR_VERIFY=1 ./u > u.out 2> u.err; rc=$?; lab="unset"
        else
            env TK_LOG_LEVEL="${lv}" TK_STR_VERIFY=1 ./u > u.out 2> u.err; rc=$?; lab="${lv}"
        fi
        got="$(msgs u.err)"
        if [ "${rc}" -eq 0 ] && [ "${got}" = "${want}" ]; then
            pass "setlevel(\"verbose\") with TK_LOG_LEVEL=${lab}: writes '${want}'"
        else
            fail "setlevel(\"verbose\") with TK_LOG_LEVEL=${lab} (rc=${rc}): got '${got}' want '${want}'"
        fi
    done
fi

if ! "${TKC}" --allow-all --out k k.tk > k.log 2>&1; then
    fail "k.tk did not compile"; sed 's/^/        /' k.log | head -6
else
    env TK_LOG_LEVEL=ERROR TK_STR_VERIFY=1 ./k > k.out 2> k.err; rc=$?
    got="$(msgs k.err)"
    if [ "${rc}" -eq 0 ] && [ "${got}" = "d i w e" ]; then
        pass "setlevel(\"debug\") overrides TK_LOG_LEVEL=ERROR: writes 'd i w e'"
    else
        fail "setlevel(\"debug\") over TK_LOG_LEVEL=ERROR (rc=${rc}): got '${got}' want 'd i w e'"
    fi
fi

echo "--------------------------------------------------------------------------------"
echo "C1640: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
