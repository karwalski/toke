#!/usr/bin/env bash
# C078_diag_positions_and_interp.sh — diagnostics carry true positions, and
# interpolation types a name by the binding in force.
#
# Stories: 127.181 (section 1), 127.186 (sections 2 and 3).  Diagnostic-
# series in content; numbered in the C range because the YAML runner is
# --check only (it cannot reach the link step where E9003 is emitted) and
# checks one field per case.
#
# ── 1. E9003 has an empty source_line (127.181) ──────────────────────────
#
# THE DEFECT.  E9003 "clang invocation failed" is emitted after code
# generation, with no toke position (offset 0, line 0).  diag.c still copied
# "the line at offset 0" out of whatever source buffer was registered:
#   * multi-module build (tkc a.tk b.tk -o bin): the driver had already
#     freed that buffer, so source_line carried heap garbage ("\u0006",
#     "\u0007", ...) -- a read of freed memory inside the machine-readable
#     record the repair loop consumes;
#   * single-module build: it carried line 1 of the program, pointing the
#     reader at a line that has nothing to do with the failure.
#
# HOW IT FORCES E9003.  -o into a directory that does not exist: the IR is
# fine, clang's link step cannot write the output.  The multi-module case is
# repeated because the garbage depends on the allocator.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_e9003sl_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C078: diagnostic positions; interpolating a re-bound name"
echo "----------------------------------------------------------"

cat > lib.tk <<'EOF'
m=pair.lib;
f=twice(x:i64):i64{<x*2};
EOF
cat > app.tk <<'EOF'
m=pair.app;
i=lib:pair.lib;
f=main():i64{<lib.twice(0)};
EOF
cat > single.tk <<'EOF'
m=single;
f=main():i64{<0};
EOF

NOWHERE="${WORK}/no/such/dir/bin"

expect_empty() {
    local name="$1"; shift
    "${TKC}" "$@" -o "${NOWHERE}" > out.log 2>&1
    local rec
    rec="$(grep '"error_code":"E9003"' out.log | head -1)"
    if [ -z "${rec}" ]; then
        echo "  FAIL: ${name}: no E9003 record"
        head -3 out.log | sed 's/^/      /'
        FAIL=$((FAIL + 1))
        return
    fi
    case "${rec}" in
        *'"source_line":""}') echo "  PASS: ${name}"; PASS=$((PASS + 1)) ;;
        *) echo "  FAIL: ${name}: ${rec##*,}"; FAIL=$((FAIL + 1)) ;;
    esac
}

expect_empty "single module" single.tk
for n in 1 2 3 4 5; do
    expect_empty "multi module, run ${n}" lib.tk app.tk
done

# ── 2. interpolation sub-parse errors point into the literal (127.186) ─────
# The segment inside "\(...)" is parsed inside a synthetic wrapper,
# `m=i;f=e():$str{<EXPR;};`.  Its lex/parse diagnostics used to carry the
# WRAPPER's offsets: line 1, a column into "m=i;...", and the source_line of
# whatever the user's line 1 was.  They must name the literal's line and a
# column inside the `\(...)`.
cat > pos.py <<'PY'
import json, sys
code, col, log = sys.argv[1], int(sys.argv[2]), sys.argv[3]
for ln in open(log):
    try:
        r = json.loads(ln)
    except ValueError:
        continue
    if r.get("error_code") != code:
        continue
    p = r["pos"]
    ok = p["line"] == 5 and p["col"] == col and "\\(" in r.get("source_line", "")
    sys.exit(0 if ok else 1)
sys.exit(1)
PY
seg_pos() {
    local name="$1" seg="$2" code="$3" col="$4"
    printf 'm=rb;\ni=io:std.io;\nf=main():i64{\n  let x=1;\n  io.println("v \\(%s)");\n  <0\n};\n' \
        "${seg}" > seg.tk
    "${TKC}" --check --diag-json seg.tk > seg.log 2>&1
    if python3 pos.py "${code}" "${col}" seg.log; then
        echo "  PASS: ${name}"; PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}"
        grep "\"${code}\"" seg.log | head -1 | cut -c1-260 | sed 's/^/      /'
        FAIL=$((FAIL + 1))
    fi
}
# line 5 is `  io.println("v \(SEG)");` -- SEG starts at column 19.
seg_pos 'E2002 in "\(x+)" is on line 5, at the end of the segment' 'x+' E2002 21
seg_pos 'W1020 in "\(#)" is at the # (line 5, col 19)'             '#'  W1020 19

# ── 3. a re-bound name is interpolated by the binding in force (127.186) ──
# `let s=@(1;2); let s="abc"; "\(s)"` checks clean (127.175); the build typed
# s by its FIRST binding, the array, and failed E4032 on a valid program.
cat > rebind.tk <<'EOF'
m=rb;
i=io:std.io;
f=main():i64{
  let s=@(1;2);
  let s="abc";
  io.println("[\(s)]");
  <0
};
EOF
rm -f rebind
if "${TKC}" rebind.tk -o rebind > rebind.log 2>&1 && [ -x rebind ] && [ "$(./rebind)" = "[abc]" ]; then
    echo "  PASS: a re-bound name interpolates its current binding"; PASS=$((PASS + 1))
else
    echo "  FAIL: a re-bound name: $(grep -o '"message":"[^"]*"' rebind.log | head -1)"
    FAIL=$((FAIL + 1))
fi

echo ""
echo "C078: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
