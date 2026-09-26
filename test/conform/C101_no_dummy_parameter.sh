#!/usr/bin/env bash
# C101_no_dummy_parameter.sh — a zero-argument stdlib function takes zero
# arguments in the glue, the ABI and the published interface (stories 136.14
# and 135.17b).
#
# THE DEFECT.  23 glue functions were written `tk_x_w(int64_t dummy) {
# (void)dummy; ... }`.  The interface generator then published the slot as a
# real parameter -- `time.nowms(i64):i64`, `sys.cpucount(i64)` and 17 more --
# so the reader was told to pass an argument nobody reads, and the checker
# had to exempt the zero-argument call from its arity rule (the 127.61
# "dummy convention").  Where the exemption could not see the dummy -- an
# alias that forwards it, `tk_time_unixnow_w(dummy) { return
# tk_time_nowunix_w(dummy); }` -- the call as documented, `time.unixnow()`,
# was rejected with E4026.
#
# THE FIX.  All 23 take `(void)`; g_stdlib_decls and the .tki are
# regenerated from them, so each is declared with no parameters, and
# check-tki fails if a dummy parameter comes back.
#
# WHAT IS ASSERTED.  Every one of the zero-argument calls compiles and runs
# (time.unixnow included); the published interface declares no parameter
# for them; and a call that passes the old dummy slot is now an arity error,
# because that slot is not part of the function.
#
# Stories: 136.14, 135.17b

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

WORK="$(mktemp -d /tmp/tkc_c101_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

echo "C101: zero-argument stdlib functions take no dummy parameter"
echo "--------------------------------------"

# ── 1. the published interface declares no parameter ─────────────────────
missing="$(python3 - "${REPO_ROOT}/stdlib" <<'PY'
import json, sys
from pathlib import Path
want = {"array.newarray", "array.newstrarray", "chart.new", "db.nowms",
        "file.tempdir", "i18n.newstrarray", "mlx.isavailable",
        "process.homedir", "securemem.isavailable", "securemem.sweep",
        "str.newarray", "str.nowiso8601", "str.nowunix", "string.uuid",
        "sys.chipname", "sys.cpucount", "sys.isarm64", "sys.platform",
        "sys.totalramgb", "time.nowms", "time.nowunix", "time.unixnow"}
seen = {}
for f in Path(sys.argv[1]).glob("*.tki"):
    for e in json.loads(f.read_text()).get("exports", []):
        if e.get("kind") == "func" and e.get("name") in want:
            seen[e["name"]] = len(e.get("params", []))
bad = [f"{n}:{'absent' if n not in seen else seen[n]}"
       for n in sorted(want) if seen.get(n) != 0]
print(" ".join(bad))
PY
)"
if [ -z "${missing}" ]; then
    pass "22 zero-argument functions are declared with no parameters"
else
    fail "declared with a parameter (or absent): ${missing}"
fi

# ── 2. every zero-argument call compiles and runs ─────────────────────────
cat > z.tk <<'EOF'
m=z;
i=io:std.io;
i=s:std.str;
i=tm:std.time;
i=sy:std.sys;
i=fl:std.file;
i=pr:std.process;
i=st:std.string;
i=db:std.db;
f=pos(label:$str;n:i64):i64{
  io.println(s.concat(label;if(n>0){" ok"}el{" zero"}));
  <0
};
f=main():i64{
  pos("time.nowms";tm.nowms());
  pos("time.nowunix";tm.nowunix());
  pos("time.unixnow";tm.unixnow());
  pos("db.nowms";db.nowms());
  pos("sys.cpucount";sy.cpucount());
  pos("sys.platform";s.len(sy.platform()));
  pos("sys.chipname";s.len(sy.chipname()));
  pos("file.tempdir";s.len(fl.tempdir()));
  pos("process.homedir";s.len(pr.homedir()));
  pos("string.uuid";s.len(st.uuid()));
  pos("str.nowiso8601";s.len(s.nowiso8601()));
  <0
};
EOF
if ! "${TKC}" --allow-all --out z_bin z.tk >compile.log 2>&1; then
    fail "the zero-argument calls did not compile"
    grep -o '"error_code":"[^"]*","severity":"[^"]*","stage":"[^"]*","message":"[^"]*"' compile.log | head -2 | sed 's/^/      /'
else
    out="$(./z_bin 2>&1)"
    for n in time.nowms time.nowunix time.unixnow db.nowms sys.cpucount \
             sys.platform sys.chipname file.tempdir process.homedir \
             string.uuid str.nowiso8601; do
        if printf '%s\n' "${out}" | grep -qxF "${n} ok"; then
            pass "${n}() runs and returns a value"
        else
            fail "${n}(): got [$(printf '%s\n' "${out}" | awk -v p="${n} " 'index($0,p)==1' | head -1)]"
        fi
    done
fi

# ── 3. passing the old dummy slot is an arity error ───────────────────────
cat > d.tk <<'EOF'
m=d;
i=tm:std.time;
f=main():i64{
  let t=tm.nowms(0);
  <0
};
EOF
"${TKC}" --check d.tk >d.log 2>&1
if grep -q '"error_code":"E4026"' d.log && grep -q 'takes 0 arguments, the call passes 1' d.log; then
    pass "time.nowms(0) is E4026: the slot is not a parameter"
else
    fail "time.nowms(0) was not rejected as an arity error: $(head -c 300 d.log)"
fi

echo "--------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
