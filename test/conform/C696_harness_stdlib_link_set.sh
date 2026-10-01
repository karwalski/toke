#!/usr/bin/env bash
# C696 -- C harnesses get their stdlib link set from tkc's dependency table,
# not from a hand-written list (127.268).
#
# Twice on 2026-10-01 a stdlib dependency change broke a separately written
# conform harness at merge: 121.74 made toml.c call file_read, and every
# harness that listed toml.c without file.c stopped linking (C661, S003,
# S009) -- in scripts nobody had touched, found only when the merged tree's
# conform ran.  The dependency table already existed (src/stdlib_deps.c,
# printed by `tkc --emit-deps`); the harnesses copied it by hand instead.
#
# scripts/stdlib_link_set.sh now asks tkc for the set on a stub program that
# imports the requested modules, and the hand lists are gone.  This script
# pins the tool and keeps the lists from coming back:
#
#   1  the set equals `tkc --emit-deps` for a program importing the modules,
#      and std.toml's set carries file.c (the 121.74 dependency)
#   2  --except drops exactly the named file (a harness that #includes
#      http.c, or compiles it with fault-injection macros, supplies it)
#   3  --except of a file not in the set, and an unknown module, fail
#      loudly (non-zero, reason on stderr) rather than print a short set
#   4  --flags prints the modules' link flags (std.http: -lssl ... -lz)
#   5  no command in test/conform/*.sh names three or more stdlib C sources
#      by hand, outside the commented allowlist below
#
# Negative control: on main 13f1d3ae the script does not exist (cases 1-4
# fail) and case 5 lists the 40+ hand-listed harnesses.  The simulated
# dependency change (a stdlib file made to call into another module, the
# table updated, tkc rebuilt) was run by hand for this story: the converted
# harnesses still link, the old hand lists do not -- see the story record.
#
# Story: 127.268

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
LS="${C696_LINKSET:-${REPO_ROOT}/scripts/stdlib_link_set.sh}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found"; exit 0; }
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
SA="$(cd "${S}" && pwd)"

# 1: the set is tkc's own
printf 'm=p;\ni=t:std.toml;\ni=h:std.http;\nf=main():i64{<0};\n' > "${WORK}/p.tk"
TKC_STDLIB_DIR="${SA}" "${TKC}" --emit-deps "${WORK}/p.tk" 2>/dev/null \
    | awk '/^---$/ { exit } NF' > "${WORK}/want"
bash "${LS}" --tkc "${TKC}" --stdlib "${S}" toml http > "${WORK}/got" 2>"${WORK}/err"
expect "1a the set for toml+http is exactly tkc --emit-deps' list" \
    "$( [ -s "${WORK}/want" ] && cmp -s "${WORK}/want" "${WORK}/got" && echo same || echo "differs ($(wc -l < "${WORK}/got") vs $(wc -l < "${WORK}/want") lines)")" "same"
bash "${LS}" --tkc "${TKC}" --stdlib "${S}" toml > "${WORK}/toml" 2>/dev/null
expect "1b std.toml's set carries file.c, tk_runtime.c and the vendored parser" \
    "$(grep -c -x -e "${SA}/file.c" -e "${SA}/tk_runtime.c" -e "${SA}/../../stdlib/vendor/tomlc99/toml.c" "${WORK}/toml")" "3"

# 2: --except
bash "${LS}" --tkc "${TKC}" --stdlib "${S}" --except http.c http > "${WORK}/ex" 2>/dev/null
expect "2 --except http.c drops http.c and nothing else" \
    "$(bash "${LS}" --tkc "${TKC}" --stdlib "${S}" http 2>/dev/null | grep -v -x -F "${SA}/http.c" | cmp -s - "${WORK}/ex" && echo ok; grep -c -x -F "${SA}/http.c" "${WORK}/ex")" \
    "ok
0"

# 3: failures are loud
bash "${LS}" --tkc "${TKC}" --stdlib "${S}" --except nosuch.c http > "${WORK}/o3" 2> "${WORK}/e3"; r1=$?
bash "${LS}" --tkc "${TKC}" --stdlib "${S}" nosuchmodule > "${WORK}/o4" 2> "${WORK}/e4"; r2=$?
expect "3 a bad --except and an unknown module: non-zero, nothing on stdout, reason on stderr" \
    "${r1} $(wc -c < "${WORK}/o3" | tr -d ' ') $(grep -c 'not in the link set' "${WORK}/e3") ${r2} $(wc -c < "${WORK}/o4" | tr -d ' ') $(grep -c 'E2030' "${WORK}/e4")" \
    "1 0 1 1 0 1"

# 4: --flags
expect "4 --flags for std.http: -lssl -lcrypto -lz among them" \
    "$(bash "${LS}" --tkc "${TKC}" --stdlib "${S}" --flags http 2>/dev/null | grep -c -x -e -lssl -e -lcrypto -e -lz)" "3"

# 5: no hand lists in test/conform
# Allowlist: path -> why the command is not a link list.
cat > "${WORK}/allow" <<'ALLOW'
S007_http_abandoned_request.sh	awk reads these sources to count call sites; nothing is linked
C696_harness_stdlib_link_set.sh	this file
ALLOW
python3 - "${REPO_ROOT}/test/conform" "${WORK}/allow" > "${WORK}/hand" <<'PY'
import glob, os, re, sys
d, allow = sys.argv[1], {l.split('\t')[0] for l in open(sys.argv[2]) if l.strip()}
pat = re.compile(r'(?:\$\{?[A-Za-z_]*\}?|src/stdlib)/([a-z_0-9]+)\.c\b')
for f in sorted(glob.glob(os.path.join(d, '*.sh'))):
    if os.path.basename(f) in allow: continue
    lines = open(f, errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        start, buf = i, lines[i]
        while buf.endswith('\\') and i + 1 < len(lines):
            i += 1; buf = buf[:-1] + ' ' + lines[i].strip()
        if not buf.lstrip().startswith('#'):
            names = {m for m in pat.findall(buf) if os.path.exists(os.path.join(d, '..', '..', 'src', 'stdlib', m + '.c'))}
            if len(names) >= 3:
                print(f"{os.path.basename(f)}:{start + 1}: {' '.join(sorted(names))}")
        i += 1
PY
expect "5 no test/conform command hand-lists 3+ stdlib C sources (use scripts/stdlib_link_set.sh)" \
    "$(cat "${WORK}/hand")" ""

echo "C696: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
