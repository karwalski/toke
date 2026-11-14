#!/usr/bin/env bash
# C134_build_all_per_triple.sh — `make build-all` builds each triple into its
# own object tree, and names every triple it could not build.
#
# Story: 135.3i.
#
# THE DEFECT.  build-all was three recursive makes that all wrote the SAME
# in-tree objects.  Once objects remembered their flags (136.62) that made a
# build-all which could succeed corrupt the host build: whichever triple ran
# last left the tree holding foreign objects.  And on a host with no sysroot
# the only signal was a missing <stdio.h>.
#
# ASSERTED (cheap: nothing here compiles the compiler):
#   1. ISOLATION.  For every triple, every file `make -n -B build-cross-<t>`
#      would write (-o) lies under build/cross/<t>/, and no two triples share
#      an output path.  Checked from make's own dry run, so it is the rules
#      that are tested, not a description of them.
#   2. NO SYSROOT = NAMED SKIP.  scripts/build_all.sh on a Linux triple with a
#      non-existent sysroot prints a SKIP line naming the triple and the
#      variable to set, its final line says INCOMPLETE/SKIPPED, and it exits 1
#      -- and 0 under BUILD_ALL_ALLOW_SKIP=1 with the same SKIPPED line.
#   3. HEADERS BUT NO LINK = NAMED SKIP.  A sysroot holding only stub headers
#      passes the compile probe and is skipped at the link probe, by name
#      (2 and 3 run with clang where CC is gcc).  3b: a CC that rejects --target is
#      skipped as unable to cross-compile, not as a sysroot without headers.
#   4. FORMAT IS VERIFIED.  With a MAKE shim that "builds" a host-native
#      binary: under the host's own triple it is BUILT; under a triple whose
#      format differs it is a FAIL, not a pass.
#
# Negative control for 1: C134_MAKEVARS='CROSS_DIR=.' puts the cross objects
# in the host tree, and this suite must fail.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MAKE_BIN="${MAKE:-make}"
CC="${CC:-cc}"
cd "${REPO_ROOT}" || exit 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c134_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C134: build-all per-triple object trees and named skips (135.3i)"
echo "--------------------------------------------------"

# ── 1. isolation, from make's dry run ─────────────────────────────────────
TRIPLES="$("${MAKE_BIN}" --no-print-directory -p -n -f Makefile 2>/dev/null \
           | awk -F' = ' '$1=="CROSS_TRIPLES"{print $2; exit}')"
if [ -z "${TRIPLES}" ]; then
    fail "could not read CROSS_TRIPLES from the Makefile"
else
    : > "${WORK}/all_outputs"
    for t in ${TRIPLES}; do
        # shellcheck disable=SC2086
        "${MAKE_BIN}" --no-print-directory -n -B ${C134_MAKEVARS:-} "build-cross-${t}" \
            > "${WORK}/dry_${t}" 2>&1
        awk '{for(i=1;i<NF;i++) if($i=="-o") print $(i+1)}' "${WORK}/dry_${t}" \
            > "${WORK}/out_${t}"
        n="$(awk 'END{print NR}' "${WORK}/out_${t}")"
        bad="$(awk -v p="build/cross/${t}/" 'index($0,p)!=1' "${WORK}/out_${t}" | sed -n '1p')"
        tgt="$(awk -v t="--target=${t}" '{for(i=1;i<=NF;i++) if($i==t){c++; break}} END{print c+0}' "${WORK}/dry_${t}")"
        if [ "${n}" -lt 2 ]; then
            fail "${t}: the dry run names ${n} output files -- expected every object and the binary"
        elif [ -n "${bad}" ]; then
            fail "${t}: writes '${bad}', outside build/cross/${t}/ (of ${n} outputs)"
        elif [ "${tgt}" -lt "${n}" ]; then
            fail "${t}: only ${tgt} of ${n} commands carry --target=${t}"
        else
            pass "${t}: all ${n} outputs under build/cross/${t}/, every command --target=${t}"
        fi
        cat "${WORK}/out_${t}" >> "${WORK}/all_outputs"
    done
    dups="$(sort "${WORK}/all_outputs" | uniq -d | awk 'END{print NR}')"
    if [ "${dups}" -eq 0 ]; then pass "no output path is shared between triples"
    else fail "${dups} output paths shared between triples"; fi
fi

# ── 2. no sysroot: named SKIP, exit 1; allow-skip: exit 0, still named ──────
run_ba() {  # env... -- triple ; writes ${WORK}/ba.log, echoes exit status
    env "$@" CC="${CC}" MAKE="${MAKE_BIN}" bash scripts/build_all.sh "${T}" \
        > "${WORK}/ba.log" 2>&1
    echo $?
}
last() { awk 'NF{l=$0} END{print l}' "${WORK}/ba.log"; }
has()  { awk -v s="$1" 'index($0,s){f=1} END{exit f?0:1}' "${WORK}/ba.log"; }

# 127.439: cross-building needs a compiler that takes --target.  On Linux
# `cc` is gcc, which rejects it, so the probes failed for THAT reason and
# cases 2-3 never reached the sysroot and link checks they are about.  They
# run with clang there; build_all.sh names the gcc case itself (3b).
CC3="${CC}"
if ! "${CC3}" --target=x86_64-linux-gnu -E -x c /dev/null >/dev/null 2>&1; then CC3=clang; fi
command -v "${CC3}" >/dev/null 2>&1 || fail "cases 2-3: no compiler that accepts --target (CC=${CC}, and no clang on PATH)"
T=x86_64-linux-gnu
rc="$(CC="${CC3}" run_ba TK_SYSROOT_x86_64_linux_gnu=/nonexistent/c134)"
if [ "${rc}" = 1 ] && has "build-all: SKIP ${T}: sysroot /nonexistent/c134" \
   && last | awk -v t="${T}" 'index($0,"INCOMPLETE") && index($0,"SKIPPED") && index($0,t){f=1} END{exit f?0:1}'; then
    pass "bad sysroot: exit 1, SKIP line and final line both name ${T}"
else
    fail "bad sysroot: rc=${rc}, last line: $(last)"
fi
rc="$(CC="${CC3}" run_ba TK_SYSROOT_x86_64_linux_gnu=/nonexistent/c134 BUILD_ALL_ALLOW_SKIP=1)"
if [ "${rc}" = 0 ] && last | awk 'index($0,"SKIPPED") && index($0,"BUILD_ALL_ALLOW_SKIP=1"){f=1} END{exit f?0:1}'; then
    pass "BUILD_ALL_ALLOW_SKIP=1: exit 0, final line still says SKIPPED"
else
    fail "BUILD_ALL_ALLOW_SKIP=1: rc=${rc}, last line: $(last)"
fi

# ── 3. headers but no libraries: the LINK probe skips it, by name ───────────
SR="${WORK}/stub_sysroot"
mkdir -p "${SR}/usr/include"
printf 'int printf(const char *, ...);\n' > "${SR}/usr/include/stdio.h"
printf 'const char *zlibVersion(void);\n' > "${SR}/usr/include/zlib.h"
printf 'double sqrt(double);\n'          > "${SR}/usr/include/math.h"
rc="$(CC="${CC3}" run_ba TK_SYSROOT_x86_64_linux_gnu="${SR}")"
if [ "${rc}" = 1 ] && has "build-all: SKIP ${T}: headers found but the link failed"; then
    pass "stub sysroot: passes the compile probe, SKIPPED at the link probe by name"
else
    fail "stub sysroot: rc=${rc}; log: $(sed -n '1,6p' "${WORK}/ba.log" | tr '\n' '|')"
fi
# 3b. a compiler with no --target is named as such, not as a missing sysroot.
mkdir -p "${WORK}/gccish"
cat > "${WORK}/gccish/cc" <<'SH'
#!/bin/sh
# gcc's answer to --target, and nothing else
for a in "$@"; do case "$a" in
  --target=*) echo "cc: error: unrecognized command-line option '$a'" >&2; exit 1 ;;
esac; done
exit 0
SH
chmod +x "${WORK}/gccish/cc"
rc="$(CC="${WORK}/gccish/cc" run_ba TK_SYSROOT_x86_64_linux_gnu="${SR}")"
if [ "${rc}" = 1 ] && has "build-all: SKIP ${T}: CC=${WORK}/gccish/cc cannot cross-compile"; then
    pass "a CC without --target: SKIPPED by name as unable to cross-compile"
else
    fail "a CC without --target: rc=${rc}; log: $(sed -n '1,6p' "${WORK}/ba.log" | tr '\n' '|')"
fi

# ── 4. the binary's format is verified, with a MAKE shim ────────────────────
case "$(uname -s)/$(uname -m)" in
    Darwin/arm64)  HOST_T=aarch64-apple-macos; OTHER_T=x86_64-linux-gnu ;;
    Darwin/x86_64) HOST_T=x86_64-apple-macos;  OTHER_T=x86_64-linux-gnu ;;
    Linux/x86_64)  HOST_T=x86_64-linux-gnu;    OTHER_T=aarch64-apple-macos ;;
    Linux/aarch64) HOST_T=aarch64-linux-gnu;   OTHER_T=x86_64-linux-gnu ;;
    *)             HOST_T=""; OTHER_T="" ;;
esac
if [ -z "${HOST_T}" ]; then
    echo "CONFORM-SKIP: C134 part 4 has no host-triple mapping for $(uname -s)/$(uname -m)"
    fail "part 4 not run on this host"
else
    printf 'int main(void){return 0;}\n' > "${WORK}/n.c"
    cat > "${WORK}/make_shim" <<EOF
#!/usr/bin/env bash
# Fake make: host-default probe flags; "build" = a host-native binary.
for a in "\$@"; do case "\$a" in
  cross-flags-*) printf 'CFLAGS=\nLDFLAGS=\nSYSROOT=\n'; exit 0 ;;
  build-cross-*) t="\${a#build-cross-}"; mkdir -p "${WORK}/cross/\$t"
                 exec "${CC}" -o "${WORK}/cross/\$t/toke" "${WORK}/n.c" ;;
esac; done
exit 2
EOF
    chmod +x "${WORK}/make_shim"
    shim() { CC="${CC}" MAKE="${WORK}/make_shim" CROSS_DIR="${WORK}/cross" \
             bash scripts/build_all.sh "$1" > "${WORK}/ba.log" 2>&1; echo $?; }
    rc="$(shim "${HOST_T}")"
    if [ "${rc}" = 0 ] && has "build-all: ${HOST_T}: BUILT"; then
        pass "host-format binary under ${HOST_T}: BUILT, exit 0"
    else
        fail "host-format binary under ${HOST_T}: rc=${rc}, last: $(last)"
    fi
    rc="$(shim "${OTHER_T}")"
    if [ "${rc}" = 1 ] && has "build-all: FAIL ${OTHER_T}:"; then
        pass "host-format binary claimed as ${OTHER_T}: FAIL, exit 1"
    else
        fail "wrong-format binary under ${OTHER_T}: rc=${rc}, last: $(last)"
    fi
fi

echo "Results: ${PASS} passed, ${FAIL} failed"
# Positive assertion: at least the fixed checks ran (4 fixed + 1 per triple + 1).
[ $((PASS + FAIL)) -ge 7 ] || { echo "C134: only $((PASS + FAIL)) checks ran"; exit 1; }
[ "${FAIL}" -eq 0 ]
