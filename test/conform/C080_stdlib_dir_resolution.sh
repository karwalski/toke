#!/usr/bin/env bash
# C080_stdlib_dir_resolution.sh — tkc finds its stdlib from any working
# directory, and says so plainly when it cannot.
#
# Story: 127.179.
#
# THE DEFECT.  Every stdlib consumer (names.c's .tki loader, llvm.c's linker)
# read TKC_STDLIB_DIR from the environment, else the baked -DTKC_STDLIB_DIR,
# and opened a relative value against the CURRENT DIRECTORY.  release.yml
# baked "./src/stdlib", so a released tkc worked only when run from inside its
# own extracted directory.  Anywhere else `tkc --check` on a std.db program
# failed `E3011 identifier 'row' is not declared` and a build failed to link;
# a wrong or missing TKC_STDLIB_DIR did the same.  ooke's CI was red for four
# months on this with nobody able to read why.
#
# NOW main() resolves the directory once -- environment, then an absolute
# baked path, then paths beside the executable -- and exports it absolute;
# if none holds a stdlib it emits E9022 and nothing else.
#
# HOW.  Case 2 and 3 need a compiler whose baked path is RELATIVE, the way a
# release is built.  Only src/main.c consults the baked path before exporting
# the resolved one, so this relinks the tree's objects with main.c recompiled
# under -DTKC_STDLIB_DIR='"src/stdlib"' -- seconds, not a full rebuild -- and
# lays it out like the release tarball (tkc beside src/stdlib and stdlib/).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"

PASS=0
FAIL=0

if [ ! -x "${TKC}" ]; then
    echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d /tmp/tkc_stdlibdir_XXXXXX)"
trap 'rm -rf "${WORK}"' EXIT

echo "C080: the stdlib directory is resolved, or E9022 says it is missing"
echo "------------------------------------------------------------------"

pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }

mkdir -p "${WORK}/elsewhere"
cd "${WORK}/elsewhere"
# `row` is declared only by std.db's interface: the name the old failure named.
cat > dbp.tk <<'EOF'
m=app;
i=db:std.db;
i=io:std.io;
f=getname():str{
  let r=db.one("SELECT name FROM t";@());
  let rw=mt r {$ok:v v;$err:e $dbrow{cols:@()}};
  let nm=mt row.str(rw;"name") {$ok:v v;$err:e "?"};
  < nm
};
f=main():i64{io.println("resolved");<0};
EOF

# only_e9022 NAME LOG RC — the run failed with E9022 and no other error.
only_e9022() {
    local name="$1" log="$2" rc="$3"
    local codes
    codes="$(grep -o '"error_code":"E[0-9]*"' "${log}" | sort -u | tr '\n' ' ')"
    if [ "${rc}" -ne 0 ] && [ "${codes}" = '"error_code":"E9022" ' ]; then
        pass "${name}"
    else
        fail "${name}: rc=${rc} codes=[${codes}]"
        grep -v '^{' "${log}" | head -2 | sed 's/^/      /'
    fi
}

# ── 1. a TKC_STDLIB_DIR that is not a stdlib directory ────────────────────
rc=0; TKC_STDLIB_DIR="${WORK}/no-such-dir" "${TKC}" --check --diag-json dbp.tk > env.log 2>&1 || rc=$?
only_e9022 "bad TKC_STDLIB_DIR is E9022, not E3011" env.log "${rc}"

# ── build a relative-baked compiler, release layout ─────────────────────────
printf 'print-objs:\n\t@echo $(OBJS)\n' > "${WORK}/objs.mk"
OBJS="$(make -s -C "${REPO_ROOT}" -f Makefile -f "${WORK}/objs.mk" print-objs 2>/dev/null)"
REL="${WORK}/release"
mkdir -p "${REL}/src" "${WORK}/bare/bin"
ok=1
"${CC}" -std=c99 -D_GNU_SOURCE -DTKC_STDLIB_DIR='"src/stdlib"' \
    -I"${REPO_ROOT}/src" -c "${REPO_ROOT}/src/main.c" -o "${WORK}/main_rel.o" 2> cc.log || ok=0
if [ "${ok}" -eq 1 ]; then
    others=()
    for o in ${OBJS}; do [ "${o}" = src/main.o ] || others+=("${REPO_ROOT}/${o}"); done
    "${CC}" -o "${REL}/tkc" "${others[@]}" "${WORK}/main_rel.o" -lm -lz -lpthread 2>> cc.log || ok=0
fi
if [ "${ok}" -ne 1 ] || [ ! -x "${REL}/tkc" ]; then
    fail "could not relink a relative-baked tkc"; sed 's/^/      /' cc.log | head -5
    echo ""; echo "C080: ${PASS} passed, ${FAIL} failed"; exit 1
fi
ln -s "${REPO_ROOT}/src/stdlib" "${REL}/src/stdlib"
ln -s "${REPO_ROOT}/stdlib"     "${REL}/stdlib"
cp "${REL}/tkc" "${WORK}/bare/bin/tkc"

# ── 2. run from another directory: resolved beside the executable ─────────
unset TKC_STDLIB_DIR
rc=0; "${REL}/tkc" --check --diag-json dbp.tk > chk.log 2>&1 || rc=$?
if [ "${rc}" -eq 0 ]; then pass "release layout, --check from elsewhere"
else fail "release layout, --check from elsewhere: rc=${rc}"; grep -o '"message":"[^"]*"' chk.log | head -2 | sed 's/^/      /'; fi

rm -f app
rc=0; "${REL}/tkc" dbp.tk -o app > build.log 2>&1 || rc=$?
if [ "${rc}" -eq 0 ] && [ -x app ] && [ "$(./app)" = "resolved" ]; then
    pass "release layout, build and run from elsewhere"
else
    fail "release layout, build from elsewhere: rc=${rc}"
    grep -v '^{' build.log | head -2 | sed 's/^/      /'
fi

# ── 3. no stdlib anywhere: E9022, not E3011 ──────────────────────────────
rc=0; "${WORK}/bare/bin/tkc" --check --diag-json dbp.tk > bare.log 2>&1 || rc=$?
only_e9022 "no stdlib beside the executable is E9022" bare.log "${rc}"

# ── 4. the purely syntactic modes do not need a stdlib ────────────────────
rc=0; "${WORK}/bare/bin/tkc" --fmt dbp.tk > /dev/null 2> fmt.log || rc=$?
if [ "${rc}" -eq 0 ]; then pass "--fmt works without a stdlib"
else fail "--fmt without a stdlib: rc=${rc}"; head -2 fmt.log | sed 's/^/      /'; fi

echo ""
echo "C080: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
