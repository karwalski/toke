#!/usr/bin/env bash
# C095_codegen_failure_names_the_decl.sh — when clang or ld rejects the
# emitted module, E9003 names the toke declaration it came from.
#
# Story: 137.13.
#
# THE DEFECT.  E9003 was "clang invocation failed with exit code 1" at line 0,
# file-level only.  clang's own message named a line of a deleted /tmp .ll,
# and ld's named a mangled symbol; nothing named a toke function or line.  A
# downstream project built three static analysers to recover what the
# compiler already knew.
#
# NOW every function definition, extern and lifted closure is recorded as it
# is emitted (IR symbol -> toke name, file, line).  clang's stderr is captured
# (and still copied to ours), the `<x>.ll:<line>:` it reports is mapped to the
# enclosing `define` in the finished .ll, and an undefined symbol from ld is
# mapped through the IR that references it.  E9003 then carries
# "... while compiling f=<name> at <file>:<line>" and the declaration's
# position in pos/line/col/offset.  An unmappable failure stays at line 0.
#
# HOW IT FORCES THE FAILURES.  Real toke programs, no injected IR:
#   * `let g=&add; g(1;2)` (124.0c) lowers to a call on an undefined IR value,
#     which clang rejects with a line number;
#   * an extern `f=nosuchcfn(x:i64):i64;` that nothing defines fails in ld;
#   * `-o` into a directory that does not exist fails with no IR involved.

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

WORK="$(mktemp -d /tmp/tkc_e9003decl_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "C095: E9003 names the toke declaration clang or ld failed on"
echo "------------------------------------------------------------"

cat > fp.tk <<'EOF'
m=fp;
f=add(a:i64;b:i64):i64{<a+b};
f=helper():i64{
  let g=&add;
  <g(1;2)
};
f=main():i64{<helper()};
EOF
cat > ext.tk <<'EOF'
m=ext;
f=nosuchcfn(x:i64):i64;
f=user(n:i64):i64{
  <nosuchcfn(n)+1
};
f=main():i64{<user(1)};
EOF
cat > lib.tk <<'EOF'
m=pair.lib;
f=add(a:i64;b:i64):i64{<a+b};
f=twice(x:i64):i64{
  let g=&add;
  <g(x;x)
};
EOF
cat > app.tk <<'EOF'
m=pair.app;
i=lib:pair.lib;
f=main():i64{<lib.twice(2)};
EOF
cat > clos.tk <<'EOF'
m=cl;
f=add(a:i64;b:i64):i64{<a+b};
f=main():i64{
  let k=5;
  let h=fn(x:i64):i64{let g=&add;<g(x;k)};
  <h(1)
};
EOF
cat > ok.tk <<'EOF'
m=ok;
f=main():i64{<0};
EOF

# check.py LOG FILE LINE COL SUBSTR... — the E9003 record's file/line/col
# match and its message contains every SUBSTR.
cat > check.py <<'PY'
import json, sys
log, file, line, col, subs = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5:]
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
r = recs[0]; p = r["pos"]; msg = r["message"]
bad = []
if r["file"] != file: bad.append("file=%r" % r["file"])
if p["line"] != line or p["col"] != col: bad.append("pos=%d:%d" % (p["line"], p["col"]))
for s in subs:
    if s not in msg: bad.append("message lacks %r" % s)
if bad:
    print("; ".join(bad) + " | message: " + msg); sys.exit(1)
PY

case_() {   # NAME LOG FILE LINE COL SUBSTR...
    local name="$1" log="$2"; shift 2
    local why
    if why="$(python3 check.py "${log}" "$@")"; then echo "  PASS: ${name}"; PASS=$((PASS + 1))
    else echo "  FAIL: ${name}: ${why}"; FAIL=$((FAIL + 1)); fi
}

# ── 1. clang rejects an IR line: mapped to the enclosing f= ───────────────
"${TKC}" fp.tk -o fpb > fp.log 2>&1
case_ "clang IR error names f=helper at fp.tk:3" fp.log fp.tk 3 1 \
      "while compiling f=helper at fp.tk:3" "use of undefined value"
if grep -q '"source_line":"f=helper():i64{"' fp.log; then
    echo "  PASS: source_line is the declaration's line"; PASS=$((PASS + 1))
else echo "  FAIL: source_line is not the declaration's line"; FAIL=$((FAIL + 1)); fi
if grep -q "error: use of undefined value" fp.log; then
    echo "  PASS: clang's own message still reaches stderr"; PASS=$((PASS + 1))
else echo "  FAIL: clang's own message no longer reaches stderr"; FAIL=$((FAIL + 1)); fi

# ── 2. ld: undefined symbol, mapped through the IR that references it ─────
"${TKC}" ext.tk -o extb > ext.log 2>&1
case_ "ld undefined symbol names f=user at ext.tk:3" ext.log ext.tk 3 1 \
      "while compiling f=user at ext.tk:3" "declared as f=nosuchcfn at ext.tk:2"

# ── 3. multi-module: the record names the module the IR came from ─────────
"${TKC}" lib.tk app.tk -o mmb > mm.log 2>&1
case_ "multi-module names f=twice at lib.tk:3" mm.log lib.tk 3 1 \
      "while compiling f=twice at lib.tk:3"

# ── 4. a lifted closure maps to the closure's own line ────────────────────
"${TKC}" clos.tk -o clb > cl.log 2>&1
case_ "closure body maps to clos.tk:5" cl.log clos.tk 5 9 \
      "while compiling closure at clos.tk:5"

# ── 5. a failure no declaration caused stays unattributed ─────────────────
"${TKC}" ok.tk -o "${WORK}/no/such/dir/bin" > nowhere.log 2>&1
if python3 check.py nowhere.log ok.tk 0 0 "clang invocation failed" > nw.out \
   && ! grep -q "while compiling" nowhere.log; then
    echo "  PASS: unwritable output: E9003 at line 0, no declaration named"; PASS=$((PASS + 1))
else echo "  FAIL: unwritable output: $(cat nw.out)"; FAIL=$((FAIL + 1)); fi

echo ""
echo "C095: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
