#!/usr/bin/env bash
# C712_e9003_clang_not_on_path.sh — when clang cannot be started at all, E9003
# says clang is missing and how to get it, not "exit code 127".
#
# Ported from PR #1 (karwalski/toke, a46aebf5).
#
# THE DEFECT.  tkc runs clang (fork + execvp, 121.1b) to compile and link
# every program.  With clang not installed or not on PATH, execvp fails, the
# child exits 127, and E9003 read "clang invocation failed with exit code
# 127" -- naming neither the tool nor the remedy, on a fresh Linux box the
# first thing a new user sees.
#
# NOW exit 127 with clang having written nothing is reported as
# "clang not found on PATH; tkc runs clang to compile and link every program
# (install clang, ...)".  Same code, same schema.
#
# CASES.
#   1. The clang to run (TKC_CC here) cannot be started: exactly one E9003,
#      the could-not-be-started message, and no "exit code 127" (121.28).
#   2. Control, PATH normal, clang fails for its own reason (-o into a
#      directory that does not exist): the message is still "clang invocation
#      failed with exit code N" -- the special case is narrow.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PY="$(command -v python3)"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi
if ! command -v clang >/dev/null 2>&1; then
    echo "FAIL: clang is not on PATH here; case 2 needs it"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_noclang_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
mkdir emptybin

echo "C712: E9003 says clang is missing when it cannot be started"
echo "------------------------------------------------------------"

cat > ok.tk <<'EOF'
m=ok;
f=main():i64{<0};
EOF

# check.py LOG WANT [REJECT] -- exactly one E9003 record, error severity, its
# message contains WANT and (if given) not REJECT.
cat > check.py <<'PY'
import json, sys
log, want = sys.argv[1], sys.argv[2]
reject = sys.argv[3] if len(sys.argv) > 3 else None
recs = []
for ln in open(log, errors="replace"):
    try:
        r = json.loads(ln)
    except ValueError:
        continue
    if r.get("error_code") == "E9003":
        recs.append(r)
if len(recs) != 1:
    print("want exactly one E9003 record, got %d" % len(recs)); sys.exit(1)
r = recs[0]; msg = r.get("message", "")
if r.get("severity") != "error":
    print("severity=%r" % r.get("severity")); sys.exit(1)
if want not in msg:
    print("message lacks %r | message: %s" % (want, msg)); sys.exit(1)
if reject and reject in msg:
    print("message has %r | message: %s" % (reject, msg)); sys.exit(1)
PY

verdict() {
    local name="$1" rc="$2" log="$3"; shift 3
    local why
    if [ "${rc}" -eq 0 ]; then
        echo "  FAIL: ${name}: tkc exited 0"; FAIL=$((FAIL + 1)); return
    fi
    if why="$("${PY}" check.py "${log}" "$@")"; then
        echo "  PASS: ${name}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: ${why}"; FAIL=$((FAIL + 1))
    fi
}

# 121.28 (owner decision §2.10): tkc runs the clang found at BUILD time by
# its absolute path, or TKC_CC -- an empty PATH no longer stops a build (C706
# case 6).  The exec failure this test guards is now a clang that passes
# main.c's check (an executable file) but cannot be started: a script whose
# interpreter does not exist.  "not found on PATH" is kept for a tkc built
# where no clang was found, which this tree cannot construct.
printf '#!%s/no/such/interpreter\n' "${WORK}" > "${WORK}/emptybin/clang"
chmod +x "${WORK}/emptybin/clang"
rc=0; env TKC_CC="${WORK}/emptybin/clang" "${TKC}" ok.tk -o okbin > c1.log 2>&1 || rc=$?
verdict "clang cannot be started: E9003 says so" "${rc}" c1.log \
        "clang could not be started" "exit code 127"

rc=0; "${TKC}" ok.tk -o nodir/okbin > c2.log 2>&1 || rc=$?
verdict "clang fails on its own: exit code still reported" "${rc}" c2.log \
        "clang invocation failed with exit code" "not found on PATH"

echo "------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
