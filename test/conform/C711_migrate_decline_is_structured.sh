#!/usr/bin/env bash
# C711_migrate_decline_is_structured.sh — a declined `tkc --migrate` emits a
# structured E9026 record and exits 4 (story 127.197).
#
# THE DEFECT. Both --migrate declines (127.57: the source already compiles;
# 127.160/127.161: comments it cannot keep) printed a plain line to stderr
# and exited 1 (ECOMPILE). AGENTS §6 requires a schema-conforming JSON record
# on every error path, and exit 1 is also what a failed migration returns, so
# a migration driver could not tell "declined, nothing written but the
# input" from "this failed".
#
# THE DECISION (owner, 2026-10-01 §2.6). A dedicated exit code,
# TKC_EXIT_DECLINED = 4, and a dedicated diagnostic, E9026 (end of the E9xxx
# series). The source is still echoed byte for byte.
#
# WHAT IS ASSERTED:
#   1. Clean source: exit 4, stdout verbatim, exactly one stderr line, and it
#      is a JSON record with error_code E9026, severity error, the schema's
#      required fields, no `fix`, and a message saying "nothing to migrate".
#   2. A comment decline: legacy source whose `//` comment holds `*)` (which
#      no rewrite to `(* *)` can keep, so it declines both before and after
#      127.161): exit 4, stdout verbatim, an E9026 record whose line is the
#      comment's.
#   3. --diag-text renders the same record for a human (code and message).
#   4. Not every exit is 4: legacy source with nothing in the way migrates,
#      exit 0, and emits no E9026.
#
# Negative control: run with TKC=<a compiler built before 127.197>; cases
# 1, 2 and 3 go red (exit 1, plain-text stderr, no record).
#
# Story: 127.197

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"

PASS=0
FAIL=0
expect() {
    local label="$1" actual="$2" want="$3"
    if [ "$actual" = "$want" ]; then
        echo "PASS $label"; PASS=$((PASS + 1))
    else
        echo "FAIL $label"
        echo "        expected: $want"
        echo "        got:      $actual"
        FAIL=$((FAIL + 1))
    fi
}
same() { cmp -s "$1" "$2" && echo same || echo differs; }

# record <stderr-file> <field> — the field of the single JSON record, or a
# reason it is not there.
record() {
    python3 - "$1" "$2" <<'PY'
import json, sys
lines = [l for l in open(sys.argv[1], encoding="utf-8").read().splitlines() if l.strip()]
if len(lines) != 1:
    print("lines=%d" % len(lines)); sys.exit()
try:
    d = json.loads(lines[0])
except ValueError:
    print("not-json"); sys.exit()
f = sys.argv[2]
if f == "schema":
    need = ["schema_version", "error_code", "severity", "stage", "message", "file", "pos"]
    miss = [k for k in need if k not in d or d[k] is None]
    miss += ["pos." + k for k in ("offset", "line", "col") if k not in d.get("pos", {})]
    print("ok" if not miss else "missing:" + ",".join(miss))
elif f == "fix":
    print("absent" if not d.get("fix") else "present")
elif f == "line":
    print(d.get("pos", {}).get("line"))
else:
    print(d.get(f))
PY
}

WORK="$(mktemp -d "${TMPDIR:-/tmp}/c711.XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

# ── 1. clean source ──────────────────────────────────────────────────────
cat > "${WORK}/clean.tk" <<'EOF2'
m=clean;
f=main():i64{
  (* kept *)
  <0
};
EOF2
"${TKC}" --check "${WORK}/clean.tk" >/dev/null 2>&1
expect "1-premise-compiles" "$?" 0
"${TKC}" --migrate "${WORK}/clean.tk" > "${WORK}/c.out" 2> "${WORK}/c.err"
expect "1a-exit-4" "$?" 4
expect "1b-verbatim" "$(same "${WORK}/clean.tk" "${WORK}/c.out")" same
expect "1c-code" "$(record "${WORK}/c.err" error_code)" E9026
expect "1d-severity" "$(record "${WORK}/c.err" severity)" error
expect "1e-schema" "$(record "${WORK}/c.err" schema)" ok
expect "1f-no-fix" "$(record "${WORK}/c.err" fix)" absent
expect "1g-reason" "$(record "${WORK}/c.err" message | grep -c 'nothing to migrate')" 1

# ── 2. a comment decline ────────────────────────────────────────────────
cat > "${WORK}/legacy.tk" <<'EOF2'
M=legc;
F=main():i64{
  let x=1;
  // closes early *) here
  <x
};
EOF2
"${TKC}" --check "${WORK}/legacy.tk" >/dev/null 2>&1
expect "2-premise-does-not-compile" "$([ $? -ne 0 ] && echo dirty || echo clean)" dirty
"${TKC}" --migrate "${WORK}/legacy.tk" > "${WORK}/l.out" 2> "${WORK}/l.err"
expect "2a-exit-4" "$?" 4
expect "2b-verbatim" "$(same "${WORK}/legacy.tk" "${WORK}/l.out")" same
expect "2c-code" "$(record "${WORK}/l.err" error_code)" E9026
expect "2d-line-is-the-comment" "$(record "${WORK}/l.err" line)" 4
expect "2e-schema" "$(record "${WORK}/l.err" schema)" ok

# ── 3. text rendering ───────────────────────────────────────────────────
"${TKC}" --diag-text --migrate "${WORK}/clean.tk" > /dev/null 2> "${WORK}/t.err"
expect "3a-text-exit-4" "$?" 4
expect "3b-text-names-code" "$(grep -c 'E9026' "${WORK}/t.err")" 1
expect "3c-text-is-not-json" "$(grep -c '^{' "${WORK}/t.err")" 0

# ── 4. a migration that goes ahead is not a decline ─────────────────────
cat > "${WORK}/plain.tk" <<'EOF2'
M=legp;
F=main():i64{
  let x=1;
  <x
};
EOF2
"${TKC}" --migrate "${WORK}/plain.tk" > "${WORK}/p.out" 2> "${WORK}/p.err"
expect "4a-exit-0" "$?" 0
expect "4b-no-E9026" "$(grep -c 'E9026' "${WORK}/p.err")" 0
"${TKC}" --check "${WORK}/p.out" >/dev/null 2>&1
expect "4c-output-compiles" "$?" 0

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
