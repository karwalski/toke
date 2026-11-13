#!/usr/bin/env bash
# C1331_struct_literal_imported_field_types.sh — a struct literal of an
# IMPORTED type has its field values judged against the .tki's declared field
# types (story 127.356), for a stdlib type and for a user module's exported
# type alike.  E4031, expected/got, no `fix`.
#
# Negative control: TKC=<main's toke> -- cases 1 and 3 fail (rc=0).
#
# Story: 127.356

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
# code|expected|got|fix-or-ABSENT of each error joined by ' / '; "rc=N" if none
diags() { printf '%s\n' "$1" > p.tk
    "${TKC}" --check p.tk > o 2>&1; rc=$?
    python3 - o "$rc" <<'PY'
import sys, json
out = []
for l in open(sys.argv[1]):
    try: d = json.loads(l)
    except Exception: continue
    if d.get("severity") != "error": continue
    out.append("%s|%s|%s|%s" % (d.get("error_code"), d.get("expected"), d.get("got"), d.get("fix", "ABSENT")))
print(" / ".join(out) if out else "rc=" + sys.argv[2])
PY
}

# 1-2. A stdlib record (std.time's timeparts, all i64 fields).
expect "1 std type: str into an i64 field" \
    "$(diags 'm=a;i=tm:std.time;f=main():i64{let p=$timeparts{year:"x";month:1;day:1;hour:0;min:0;sec:0};<p.month};')" \
    "E4031|i64|str|ABSENT"
expect "2 std type: matching fields stay clean" \
    "$(diags 'm=a;i=tm:std.time;f=main():i64{let p=$timeparts{year:2026;month:1;day:1;hour:0;min:0;sec:0};<p.month};')" "rc=0"

# 3-4. A user module's exported record, through its emitted interface.
cat > amod.tk <<'EOF'
m=amod;
t=$cfgrec{name:$str;n:i64;w:f64};
f=mk(x:i64):$cfgrec{<$cfgrec{name:"w";n:x;w:1.0}};
EOF
"${TKC}" --emit-interface --check amod.tk >/dev/null 2>&1
if [ ! -f amod.tki ]; then
    echo "FAIL 3-setup: --emit-interface produced no amod.tki"; FAIL=$((FAIL + 1))
else
    expect "3 user-module type: int into str, int literal into f64" \
        "$(diags 'm=b;i=a:amod;f=main():i64{let c=$cfgrec{name:1;n:2;w:3};<c.n};')" \
        "E4031|str|i64|ABSENT / E4031|f64|i64|ABSENT"
    expect "4 user-module type: matching fields stay clean" \
        "$(diags 'm=b;i=a:amod;f=main():i64{let c=$cfgrec{name:"q";n:2;w:3.0};<c.n};')" "rc=0"
fi

echo "C1331: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
