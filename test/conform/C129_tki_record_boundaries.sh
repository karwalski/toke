#!/usr/bin/env bash
# C129_tki_record_boundaries.sh — a `.tki` export record is delimited by its
# braces, not by the next "kind" substring (story 127.134a).
#
# THE DEFECT.  Seven scanners (src/llvm.c x4, src/names.c x2, src/glue_gen.c)
# found each export record with strstr(p, "\"kind\"") and bounded it at the
# NEXT "kind" substring.  A field NAMED kind — {"name":"kind"} — is that
# substring, so the record was cut off there and its later fields vanished,
# but only for the scanners that took the substring route: with such a field
# first in math.linregresult, `tkc --check` accepted `.r2` (exit 0) while
# `tkc --out` rejected it (E4025 "struct 'linregresult' has no field 'r2'").
# One compiler, one interface, two answers.  All seven now walk the exports
# array structurally (src/tki_scan.h).
#
# WHAT THIS DRIVES.  A private copy of the stdlib tree whose math.tki gains a
# field named `kind` — first in linregresult, in the middle, and behind a
# top-level free-text string holding escaped quotes (the shape gen_tki.py
# writes into eleven stdlib interfaces; an escape-blind scan lost its place
# there and found no exports at all) — then `--check` and `--out` on a
# program reading `.r2`.  Both must
# accept it.  (The value printed is not asserted: the decoy field shifts the
# declared layout away from the C glue's, which is the decoy's doing.)  The
# unmodified interface must give the same verdicts.
#
# Negative control: build tkc at the parent of the 127.134a commit — case
# "first" reports check rc 0 / out rc 2 with E4025.
#
# Story: 127.134a

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
STDLIB_SRC="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

if [ ! -x "${TKC}" ]; then
    echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c129_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C129: .tki records are delimited structurally"
echo "---------------------------------------------"

cat > "${WORK}/lr.tk" <<'EOF'
m=lr;
i=io:std.io;
i=math:std.math;
f=main():i64{
  let r=math.linreg(@(1.0;2.0;3.0);@(2.0;4.0;6.0));
  io.println("\(r.r2)");
  <0
};
EOF

# mktree NAME PYTHON-MODE — a stdlib tree whose math.tki is edited per MODE.
mktree() {
    local d="${WORK}/$1"
    mkdir -p "${d}/src"
    cp -R "${STDLIB_SRC}" "${d}/src/stdlib"
    cp -R "${STDLIB_SRC}/../../stdlib" "${d}/stdlib"
    python3 - "${d}/stdlib/math.tki" "$2" <<'PY' || return 1
import sys
p, mode = sys.argv[1], sys.argv[2]
s = open(p).read()
decoy = '        {\n          "name": "kind",\n          "type": "i64"\n        },\n'
if mode == "first":
    a = '      "name": "linregresult",\n      "fields": [\n'
elif mode == "middle":
    a = '          "name": "slope",\n          "type": "f64"\n        },\n'
elif mode == "escaped":
    # a free-text top-level string with escaped quotes before "exports", the
    # shape scripts/gen_tki.py writes into eleven stdlib interfaces; plus the
    # decoy, so the record must be found AND delimited correctly
    m = '  "module": "std.math",\n'
    if s.count(m) != 1:
        sys.exit("module anchor not found")
    s = s.replace(m, m + '  "note": "a \\"kind\\":\\"type\\" b",\n')
    a = '      "name": "linregresult",\n      "fields": [\n'
else:
    a = None
if a is not None:
    if s.count(a) != 1:
        sys.exit("injection anchor not found exactly once: " + mode)
    s = s.replace(a, a + decoy)
    if s.count('"name": "kind"') != 1:
        sys.exit("decoy did not land: " + mode)
open(p, "w").write(s)
PY
}

for mode in plain first middle escaped; do
    if ! mktree "${mode}" "${mode}"; then fail "${mode}: could not build the stdlib copy"; continue; fi
    export TKC_STDLIB_DIR="${WORK}/${mode}/src/stdlib"
    "${TKC}" --check "${WORK}/lr.tk" >"${WORK}/${mode}.chk" 2>&1; crc=$?
    "${TKC}" --allow-all --out "${WORK}/${mode}.bin" "${WORK}/lr.tk" >"${WORK}/${mode}.out" 2>&1; orc=$?
    e="$(awk '/"severity":"error"/' "${WORK}/${mode}.chk" "${WORK}/${mode}.out" | head -1 | cut -c1-200)"
    if [ "${crc}" = 0 ] && [ "${orc}" = 0 ]; then
        pass "${mode}: --check rc 0 and --out rc 0 agree"
    else
        fail "${mode}: --check rc ${crc}, --out rc ${orc}: ${e}"
    fi
done
unset TKC_STDLIB_DIR

echo ""
echo "C129: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
