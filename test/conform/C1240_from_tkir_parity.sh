#!/usr/bin/env bash
# C1240_from_tkir_parity.sh -- `tkc --from-tkir` builds the program the .tkir
# was written from, identically (story 76.1.6c).
#
# WHAT IS BEING PROVED.  --from-tkir rebuilds the parsed program from the
# .tkir's TEXT and AST sections (format 1.2) and hands it to the same name
# resolver, type checker and LLVM backend a source compile uses.  So the
# claim is exact: for every program, the LLVM IR from source and the LLVM IR
# from source -> .tkir are the same bytes.
#
#   1. scripts/diff_tkir.py --strict over the diff-codegen program set
#      (test/standalone, test/e2e, test/codegen): every program that compiles
#      from source must round-trip with byte-identical IR; nothing may be
#      refused, differ, or fail without a structured diagnostic.
#   2. end to end on a program written to stress the TEXT section -- block
#      comments, string interpolation, floats, a struct, a loop, match on a
#      sum type, a closure: build both binaries, run them, compare stdout
#      and exit status, and confirm the .tkir holds no comment text.
#   3. re-emission is a fixed point: --from-tkir x.tkir --emit-tkir writes
#      the same bytes as x.tkir.
#   4. -g from a .tkir names the ORIGINAL source file in the debug info (its
#      lines are that file's), not the .tkir.
#
# The full behavioural sweep (both binaries for every program, run and
# compared) is `make diff-tkir-run`; it costs two clang links per program, so
# this case runs it on the hand-written program only.
#
# Negative control (run 2026-11-10): in src/tkir_ast.c dec_node, rebuild
# every `*` binary node as `+` -- part 1 reports 13 of the 132 programs
# DIFFERENT and parts 2 and 3 fail too (3 FAIL).
#
# Story: 76.1.6c

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi
TKC="$(cd "$(dirname "${TKC}")" && pwd)/$(basename "${TKC}")"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1240_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C1240: --from-tkir parity with the source compile"
echo "-------------------------------------------------"

# ── 1. IR parity over the diff-codegen set ─────────────────────────────
out="$(cd "${REPO_ROOT}" && TOKE="${TKC}" python3 scripts/diff_tkir.py --strict --min 100 \
        test/standalone test/e2e test/codegen 2>&1)"
rc=$?
summary="$(printf '%s\n' "${out}" | grep '^diff_tkir:')"
if [ "${rc}" -eq 0 ] && printf '%s\n' "${summary}" | grep -q "DIFFERENT 0, UNSTRUCTURED 0"; then
    pass "IR identical for every program: ${summary#diff_tkir: }"
else
    fail "IR parity: $(printf '%s\n' "${out}" | tail -8)"
fi

# ── 2. end to end on a TEXT-section stress program ─────────────────────
cat > "${WORK}/p.tk" <<'EOF'
m=p;
i=io:std.io;
(* block comment: SECRETCOMMENT must not reach the .tkir *)
t=$pt{x:i64;y:f64};
t=$shape{$circle:f64;$sq:i64};
f=area(s:$shape):f64{
  <mt s {$circle:r r*r*3.0;$sq:n (n*n) as f64}
};
f=main():i64{
  let p=$pt{x:3;y:2.5};          (* trailing comment *)
  let s=mut.0;
  lp(let i=0;i<5;i=i+1){s=s+i*i};
  let add=fn(a:i64):i64{<a+p.x};
  let c=$shape{$circle:2.0};
  io.println("s=\(s) y=\(p.y) add=\(add(4)) a=\(area(c))");
  <s
};
EOF
if "${TKC}" "${WORK}/p.tk" --allow-all --out "${WORK}/a" >"${WORK}/a.log" 2>&1 &&
   "${TKC}" "${WORK}/p.tk" --allow-all --emit-tkir --out "${WORK}/p.tkir" >"${WORK}/e.log" 2>&1 &&
   "${TKC}" --from-tkir "${WORK}/p.tkir" --allow-all --out "${WORK}/b" >"${WORK}/b.log" 2>&1; then
    oa="$("${WORK}/a" </dev/null)"; ca=$?
    ob="$("${WORK}/b" </dev/null)"; cb=$?
    if [ "${oa}" = "${ob}" ] && [ "${ca}" = "${cb}" ] && [ -n "${oa}" ]; then
        pass "binaries agree: exit ${ca}, stdout '${oa}'"
    else
        fail "binaries differ: source exit ${ca} '${oa}' / tkir exit ${cb} '${ob}'"
    fi
else
    fail "stress program did not build both ways: $(cat "${WORK}"/a.log "${WORK}"/e.log "${WORK}"/b.log | head -c 600)"
fi
if [ -f "${WORK}/p.tkir" ] && ! /usr/bin/grep -a -q "SECRETCOMMENT\|trailing comment" "${WORK}/p.tkir"; then
    pass ".tkir carries no comment text"
else
    fail ".tkir is missing or carries comment text"
fi

# ── 3. re-emission is a fixed point ────────────────────────────────────
if "${TKC}" --from-tkir "${WORK}/p.tkir" --allow-all --emit-tkir --out "${WORK}/q.tkir" >"${WORK}/q.log" 2>&1 &&
   cmp -s "${WORK}/p.tkir" "${WORK}/q.tkir"; then
    pass "--from-tkir --emit-tkir reproduces the .tkir byte for byte"
else
    fail "re-emitted .tkir differs: $(head -c 300 "${WORK}/q.log")"
fi

# ── 4. -g names the original source ────────────────────────────────────
cp "${WORK}/p.tkir" "${WORK}/renamed.tkir"
if "${TKC}" --from-tkir "${WORK}/renamed.tkir" -g --allow-all --emit-llvm --out "${WORK}/g.ll" >"${WORK}/g.log" 2>&1 &&
   /usr/bin/grep -q 'DIFile(filename: "p.tk"' "${WORK}/g.ll" &&
   ! /usr/bin/grep -q 'renamed.tkir' "${WORK}/g.ll"; then
    pass "-g debug info names p.tk"
else
    fail "-g debug info: $(/usr/bin/grep -m2 'DIFile' "${WORK}/g.ll" 2>/dev/null) $(head -c 300 "${WORK}/g.log")"
fi

echo ""
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
