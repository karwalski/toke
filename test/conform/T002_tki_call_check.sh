#!/usr/bin/env bash
# T002_tki_call_check.sh — a call is checked against the declaration that
# introduced it, instead of being lowered to whatever symbol happens to exist
# (story 136.1).
#
# Before this story nothing compared `alias.member(...)` with anything at all.
# A call that disagreed on arity type-checked clean, reached codegen, and
# linked against a symbol with a different signature: the arguments landed in
# the wrong places and the program **corrupted silently** rather than failing.
# A member nothing provides fell through to the generic `tk_<mod>_<method>_w`
# name and surfaced as an E9003 link failure naming a mangled symbol, never
# naming the function or the line (the 127.77 shape).
#
#     tpl.renderfile("p.tkt"; vars)   (* the export is tplrenderfile/3 *)
#
# passed `--check` at exit 0 (Epic 136), and so did the nine bindings functions
# whose `.tki` arity disagrees with their ABI.
#
# The cases need a generated .tki on disk, so they cannot be expressed as a
# single-input YAML case.
#
# Story: 136.1

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
# The ~/tk/toke/tkc symlink is relinked by any concurrent `make`; $TKC lets a
# caller pin a resolved binary for the run (131.39).
TKC="${TKC:-${REPO_ROOT}/tkc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_tkicall_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

echo "T002: a call is checked against the interface and the implementation"
echo "--------------------------------------"

# The exported module.  Its .tki is machine-generated from this source, so its
# export list is complete by construction and can be enforced.
cat > tplmod.tk <<'EOF'
m=tplmod;
f=tplrenderfile(path:str;vars:str;root:str):str{
  <path
};
f=ping():i64{
  <1
};
EOF
"${TKC}" --emit-interface --check tplmod.tk >/dev/null 2>&1
if [ ! -f tplmod.tki ]; then
    echo "  FAIL: setup: --emit-interface produced no tplmod.tki"
    exit 1
fi

# check_case NAME EXPECTED_RC EXPECTED_GREP SOURCE
#   Runs `tkc --check -I . case.tk`; asserts the exit code, and (when
#   EXPECTED_GREP is non-empty) that the diagnostic stream matches it.
check_case() {
    local name="$1" exp_rc="$2" pattern="$3" src="$4"
    printf '%s\n' "${src}" > case.tk
    local out rc=0
    out="$("${TKC}" --check --diag-json -I . case.tk 2>&1)" || rc=$?
    if [ "${rc}" != "${exp_rc}" ]; then
        echo "  FAIL: ${name}: expected exit ${exp_rc}, got ${rc}"
        echo "${out}" | sed 's/^/      /' | head -5
        FAIL=$((FAIL + 1))
        return
    fi
    if [ -n "${pattern}" ] && ! printf '%s' "${out}" | grep -q "${pattern}"; then
        echo "  FAIL: ${name}: diagnostics did not match [${pattern}]"
        echo "${out}" | sed 's/^/      /' | head -5
        FAIL=$((FAIL + 1))
        return
    fi
    echo "  PASS: ${name}"
    PASS=$((PASS + 1))
}

# ── 1: arity mismatch against a generated interface.  This is the reported
#      shape: the interface declares three parameters, the call passes two,
#      and before 136.1 it compiled. ──
check_case "arity mismatch vs generated .tki" 1 '"error_code":"E4026"' '
m=t;
i=tpl:tplmod;
f=main():i64{
  tpl.tplrenderfile("p.tkt";"v");
  <0
};'

# ── 2: the diagnostic names the function and both signatures, not two bare
#      counts — a count alone does not say which side to change. ──
check_case "E4026 names the function and the declaration" 1 "the interface declares tpl.tplrenderfile(str; str; str)" '
m=t;
i=tpl:tplmod;
f=main():i64{
  tpl.tplrenderfile("p.tkt";"v");
  <0
};'

# ── 3: unknown member.  `renderfile` is not an export of tplmod; before 136.1
#      this reached the linker as a mangled symbol name. ──
check_case "unknown member vs generated .tki" 1 '"error_code":"E4027"' '
m=t;
i=tpl:tplmod;
f=main():i64{
  tpl.renderfile("p.tkt";"v";"r");
  <0
};'
check_case "E4027 names the module and the member" 1 "module .tplmod. has no exported member .renderfile." '
m=t;
i=tpl:tplmod;
f=main():i64{
  tpl.renderfile("p.tkt";"v";"r");
  <0
};'

# ── 4: the guard that matters most — a correct call must keep compiling.  A
#      check that fires wrongly is worse than one that is incomplete. ──
check_case "correct calls still compile" 0 "" '
m=t;
i=io:std.io;
i=tpl:tplmod;
f=main():i64{
  io.println(tpl.tplrenderfile("p.tkt";"v";"r"));
  <tpl.ping()
};'

# ── 5: a std.* call is judged against the *implementation*, because that is
#      what decides whether the call corrupts.  This is the shape that makes
#      `vecstore.upsert` exit 139 when called exactly as its interface
#      documents (136.4) and that lets `math.min(xs)` pass an array pointer
#      into a two-argument C function and return a garbage double (found in
#      test/stdlib/math_stats.tk by this story's sweep).  The case is pinned
#      on `str.concat`, whose two-argument glue is not under repair by any
#      Epic 136 row, so the assertion does not move when those are fixed. ──
check_case "arity mismatch vs the std implementation" 1 '"error_code":"E4026"' '
m=t;
i=s:std.str;
f=main():i64{
  s.concat("a");
  <0
};'
check_case "E4026 names the implementation arity" 1 "the implementation takes 2 arguments, the call passes 1" '
m=t;
i=s:std.str;
f=main():i64{
  s.concat("a");
  <0
};'

# ── 6: a std.* member that neither the interface nor the runtime declares.
#      This is 127.77: it used to surface as an E9003 naming the mangled
#      symbol, with no file and no line. ──
check_case "std member with no symbol" 1 '"error_code":"E4027"' '
m=t;
i=s:std.str;
f=main():i64{
  s.ne("a";"b");
  <0
};'

# ── 7 (narrowing guard): a std.* member the hand-written .tki does not declare
#      but the runtime does provide.  `str.equals` is used at 1391 sites in the
#      test-programs library and works; the interface is the incomplete side,
#      so the call must not be rejected. ──
check_case "undeclared-but-implemented std member accepted" 0 "" '
m=t;
i=s:std.str;
f=main():i64{
  if(s.equals("a";"a")){<1};
  <0
};'

# ── 8 (narrowing guard): a zero-argument toke function is written in glue as a
#      single ignored `int64_t dummy`; 23 functions use that idiom.  The caller
#      sets no register and the callee reads none, so the zero-argument call is
#      the convention, not a disagreement. ──
check_case "zero-arg call into dummy-parameter glue accepted" 0 "" '
m=t;
i=io:std.io;
i=f:std.file;
f=main():i64{
  io.println(f.tempdir());
  <0
};'

# ── 9 (narrowing guard): a sub-namespace prefix (`row.` on std.db) is not an
#      `I=` alias, so it is out of scope and must stay silent rather than being
#      reported as an unknown member of the importing module. ──
check_case "sub-namespace call left alone" 0 "" '
m=t;
i=db:std.db;
f=run():i64!DbErr{
  let r=db.one("SELECT 1 AS x";@())!DbErr;
  <row.i64(r;"x")!DbErr
};
f=main():i64{ <mt run() {$ok:v v;$err:e 1} };'

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
