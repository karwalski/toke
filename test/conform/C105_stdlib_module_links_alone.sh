#!/usr/bin/env bash
# C105_stdlib_module_links_alone.sh — every stdlib wrapper that exists is
# reachable from the module that declares it (story 136.33).
#
# THE DEFECT.  tk_web_glue.c is registered against std.http in
# src/stdlib_deps.c, and it held wrappers for OTHER modules.  A program that
# imported one of those modules without also importing std.http type-checked
# and then died at ld on a symbol that was present in the tree:
#
#     i=auth:std.auth;   auth.hash(..)      -> undefined tk_auth_hash_w
#     i=chart:std.chart; chart.serve(..)    -> undefined tk_chart_serve_w
#     i=fmt:std.fmt;     fmt.print(..)      -> undefined tk_fmt_print_w
#
# 136.40 had already moved the dataframe and dashboard halves; at d1f1ba6 the
# auth four, chart.addchart / chart.serve and fmt.print were still there.
# (136.33 also named "config" and "cache": neither has a .tki, so neither can
# be imported and there is nothing to link alone.)
#
# WHAT IS ASSERTED.
#   A. The whole class, statically.  For every function exported by every
#      stdlib/*.tki whose tk_<module>_<fn>_w wrapper is DEFINED somewhere in
#      src/stdlib, at least one defining file is in the link set
#      `tkc --emit-deps` reports for a program importing that module alone.
#      A wrapper defined nowhere is check-tki's business (the quarantine), not
#      this test's.
#   B. The named modules, behaviourally: a program importing only that module
#      builds, links and prints what the wrapper computed.
#
# Negative control: set chart's deps back to "" in src/stdlib_deps.c — A
# names chart.addchart and chart.serve, and B's chart case fails at link.
#
# Story: 136.33

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_c105_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "C105: a stdlib module links without std.http"
echo "--------------------------------------------"
echo ""
echo "A. every defined wrapper is in its own module's link set"

python3 - "${TKC}" "${REPO_ROOT}" "${WORK}" > "${WORK}/reach.log" 2>&1 <<'PY'
import json, os, pathlib, re, subprocess, sys
tkc, root, work = sys.argv[1], pathlib.Path(sys.argv[2]), pathlib.Path(sys.argv[3])
stdlib = root / "src" / "stdlib"
# std.task calls lower straight to the tk_task_* core (llvm.c), so its _w
# wrappers are never referenced and cannot fail to link.
NEVER_CALLED = {"tk_task_scope_w", "tk_task_spawn_w", "tk_task_awaitall_w",
                "tk_task_result_w", "tk_task_cancel_w"}
DEF = re.compile(r"^[A-Za-z_][\w \t*]*?\b(tk_\w+_w)\s*\([^;{]*\)\s*\{", re.M)
defs = {}
for f in stdlib.glob("*.c"):
    for m in DEF.finditer(f.read_text(errors="ignore")):
        defs.setdefault(m.group(1), set()).add(f.name)
checked = bad = 0
for t in sorted((root / "stdlib").glob("*.tki")):
    mod = t.stem
    try:
        exports = json.loads(t.read_text()).get("exports", [])
    except ValueError:
        continue
    p = work / "probe.tk"
    p.write_text("m=t;\ni=z:std.%s;\nf=main():i64{<0};\n" % mod)
    r = subprocess.run([tkc, "--emit-deps", str(p)], capture_output=True, text=True)
    if r.returncode != 0:
        print("EMIT-DEPS FAILED std.%s: %s" % (mod, r.stderr.strip()[:200]))
        bad += 1
        continue
    linked = {os.path.basename(x) for x in r.stdout.split()}
    for e in exports:
        if e.get("kind") != "func":
            continue
        sym = "tk_%s_%s_w" % (mod, e["name"].split(".")[-1])
        where = defs.get(sym)
        if not where or sym in NEVER_CALLED:
            continue
        checked += 1
        if not where & linked:
            print("UNREACHABLE std.%s %s: %s is defined only in %s"
                  % (mod, e["name"], sym, ", ".join(sorted(where))))
            bad += 1
print("checked %d wrappers, %d unreachable" % (checked, bad))
sys.exit(1 if bad or checked < 100 else 0)
PY
if [ $? -eq 0 ]; then
    echo "  PASS: $(tail -1 "${WORK}/reach.log")"
    PASS=$((PASS + 1))
else
    echo "  FAIL: a wrapper is outside its module's link set"
    sed 's/^/        /' "${WORK}/reach.log" | head -20
    FAIL=$((FAIL + 1))
fi

echo ""
echo "B. the modules 136.33 named, imported alone"

# case_ NAME WANT SOURCE — build, run, compare stdout exactly.
case_() {
    local name="$1" want="$2" src="$3"
    printf '%s\n' "${src}" > "${WORK}/t.tk"
    rm -f "${WORK}/t.bin"
    if ! "${TKC}" --allow-all --out "${WORK}/t.bin" "${WORK}/t.tk" > "${WORK}/build.log" 2>&1; then
        echo "  FAIL: ${name}: did not build"
        grep -a -o 'undefined symbol[^"]*' "${WORK}/build.log" | head -3 | sed 's/^/        /'
        FAIL=$((FAIL + 1))
        return
    fi
    local got
    got="$("${WORK}/t.bin" 2>/dev/null | tr '\n' ' ' | sed 's/ *$//')"
    if [ "${got}" = "${want}" ]; then
        echo "  PASS: ${name}"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: ${name}: printed [${got}], expected [${want}]"
        FAIL=$((FAIL + 1))
    fi
}

# auth.hash / auth.verify round-trip, and a wrong password is refused, so the
# wrappers that link are the real ones and not stubs.
case_ "std.auth alone: hash, verify, jwt, verifyjwt" "1 0 1 0" \
'm=t;
i=auth:std.auth;
i=io:std.io;
f=main():i64{
  let h=auth.hash("hunter2");
  io.println(auth.verify("hunter2";h) as $str);
  io.println(auth.verify("hunter3";h) as $str);
  let tok=auth.jwt("alice";"k3y");
  io.println(auth.verifyjwt(tok;"k3y") as $str);
  io.println(auth.verifyjwt(tok;"wrong") as $str);
  <0
};'

# chart.addchart / chart.serve refuse a null dashboard (0 and -1): what
# matters is that the program links and reaches them.
case_ "std.chart alone: addchart, serve" "0 -1" \
'm=t;
i=chart:std.chart;
i=io:std.io;
f=main():i64{
  io.println(chart.addchart(0;0) as $str);
  io.println(chart.serve(0;0) as $str);
  <0
};'

case_ "std.fmt alone: print" "42" \
'm=t;
i=fmt:std.fmt;
f=main():i64{
  fmt.print(42);
  <0
};'

# The two halves 136.40 moved, pinned here with the rest.
case_ "std.dataframe alone: fromcsv, shape" "2 2" \
'm=t;
i=df:std.dataframe;
i=io:std.io;
f=main():i64{
  let d=mt df.fromcsv("a,b\n1,2\n3,4\n"){$ok:v v;$err:e <1};
  let s=df.shape(d);
  io.println(s.rows as $str);
  io.println(s.cols as $str);
  <0
};'

case_ "std.dashboard alone: new" "built" \
'm=t;
i=dashboard:std.dashboard;
i=io:std.io;
f=main():i64{
  let d=dashboard.new("Metrics";2);
  io.println("built");
  <0
};'

echo "--------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
