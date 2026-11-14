#!/usr/bin/env bash
# build_all.sh — `make build-all`: build tkc for every triple in
# $(CROSS_TRIPLES), each into its own object tree, and say by name which
# triples were NOT built and why.  Story 135.3i.
#
# Usage (from the Makefile): bash scripts/build_all.sh <triple>...
#   env: CC, MAKE, CROSS_DIR (build/cross), BUILD_ALL_ALLOW_SKIP (0),
#        TK_SYSROOT_<triple_with_underscores>, TK_CROSS_LDFLAGS_<...>
#
# For each triple:
#   1. PROBE: compile AND link a hello-world with exactly the flags the build
#      rules will use (asked of make, `make cross-flags-<triple>`, so the two
#      cannot drift).  A compile failure means no libc headers for the triple
#      (no sysroot); a link failure means no linker/libraries for it.  Either
#      is a named SKIP -- this script never downloads anything.
#   2. BUILD: `make build-cross-<triple>` -> $CROSS_DIR/<triple>/toke.  The
#      in-tree host objects (src/**/*.o, ./toke) are not read or written.
#   3. VERIFY: file(1) must report the binary format the triple implies
#      (ELF x86-64 / ELF aarch64 / Mach-O arm64 or x86_64).  A build that
#      "succeeded" into the wrong format is a FAIL, not a pass.
#
# Exit: 0 only if every triple was built and verified.  A skipped triple
# exits 1 unless BUILD_ALL_ALLOW_SKIP=1; a failed build or verification
# always exits 1.  The last line always states built / skipped / failed.
#
# Decisions are made from exit statuses and `case` on file(1)'s text -- never
# from an empty grep (127.131).

set -uo pipefail

CC="${CC:-cc}"
MAKE="${MAKE:-make}"
CROSS_DIR="${CROSS_DIR:-build/cross}"
ALLOW_SKIP="${BUILD_ALL_ALLOW_SKIP:-0}"

PROBE_ONLY=0
if [ "${1:-}" = "--probe-only" ]; then PROBE_ONLY=1; shift; fi   # make build-preflight

[ "$#" -gt 0 ] || { echo "build_all.sh: no triples given -- refusing to pass" >&2; exit 2; }

built=(); skipped=(); failed=()
probe_dir="$(mktemp -d "${TMPDIR:-/tmp}/tkc_build_all.XXXXXX")" || exit 1
trap 'rm -rf "${probe_dir}"' EXIT
printf '#include <stdio.h>\n#include <zlib.h>\n#include <math.h>\nint main(void){printf("%%s %%f\\n", zlibVersion(), sqrt(2.0));return 0;}\n' \
    > "${probe_dir}/p.c"

expect_format() {   # triple -> shell glob that file(1)'s output must match
    case "$1" in
        x86_64-*linux*)   echo '*ELF 64-bit*x86-64*' ;;
        aarch64-*linux*)  echo '*ELF 64-bit*aarch64*' ;;
        aarch64-apple-*|arm64-apple-*) echo '*Mach-O 64-bit*arm64*' ;;
        x86_64-apple-*)   echo '*Mach-O 64-bit*x86_64*' ;;
        *)                echo '' ;;
    esac
}

for t in "$@"; do
    var="${t//-/_}"
    flags="$("${MAKE}" --no-print-directory "cross-flags-${t}")" || {
        failed+=("${t} (make cross-flags-${t} failed)"); continue; }
    cflags="$(printf '%s\n' "${flags}" | sed -n 's/^CFLAGS=//p')"
    ldflags="$(printf '%s\n' "${flags}" | sed -n 's/^LDFLAGS=//p')"
    sysroot="$(printf '%s\n' "${flags}" | sed -n 's/^SYSROOT=//p')"

    echo "build-all: ${t}: probing (${cflags}${ldflags:+ | link: ${ldflags}})"
    # shellcheck disable=SC2086
    if ! "${CC}" ${cflags} -c -o "${probe_dir}/p.o" "${probe_dir}/p.c" \
            > "${probe_dir}/cc.log" 2>&1; then
        why="$(sed -n '1{s|^.*/p\.c:[0-9]*:[0-9]*: ||;p;}' "${probe_dir}/cc.log")"
        # 127.439: gcc (cc on Linux) has no --target; that is not a missing
        # sysroot, and saying so sent a reader looking for headers.
        if grep -q -e "--target" "${probe_dir}/cc.log" \
           && grep -q -i -e "unrecognized" -e "unknown argument" "${probe_dir}/cc.log"; then
            skipped+=("${t}: CC=${CC} cannot cross-compile -- it does not accept --target; use CC=clang (${why})")
        elif [ -n "${sysroot}" ]; then
            skipped+=("${t}: sysroot ${sysroot} has no usable libc/zlib headers (${why})")
        else
            skipped+=("${t}: no sysroot -- set TK_SYSROOT_${var}=<dir> (${why})")
        fi
        continue
    fi
    # shellcheck disable=SC2086
    if ! "${CC}" ${cflags} ${ldflags} -o "${probe_dir}/p" "${probe_dir}/p.o" -lz -lm \
            > "${probe_dir}/ld.log" 2>&1; then
        why="$(sed -n '1{s|^.*/p\.o[: ]*||;p;}' "${probe_dir}/ld.log")"
        skipped+=("${t}: headers found but the link failed -- needs a linker and libraries for the triple (ld.lld; TK_CROSS_LDFLAGS_${var}) (${why})")
        continue
    fi

    if [ "${PROBE_ONLY}" = 1 ]; then built+=("${t}"); echo "build-all: ${t}: probe OK"; continue; fi
    echo "build-all: ${t}: building into ${CROSS_DIR}/${t}/"
    if ! "${MAKE}" --no-print-directory "build-cross-${t}"; then
        failed+=("${t}: make build-cross-${t} failed")
        continue
    fi
    bin="${CROSS_DIR}/${t}/toke"
    want="$(expect_format "${t}")"
    got="$(file -b "${bin}" 2>&1)"
    if [ -z "${want}" ]; then
        failed+=("${t}: built, but this script has no expected binary format for the triple -- add one")
        continue
    fi
    # shellcheck disable=SC2254
    case "${got}" in
        ${want}) built+=("${t}"); echo "build-all: ${t}: BUILT ${bin} -- ${got}" ;;
        *)       failed+=("${t}: ${bin} is '${got}', expected ${want}") ;;
    esac
done

for s in "${skipped[@]+"${skipped[@]}"}"; do echo "build-all: SKIP ${s}"; done
for f in "${failed[@]+"${failed[@]}"}";  do echo "build-all: FAIL ${f}"; done

n=$#; nb=${#built[@]}; ns=${#skipped[@]}; nf=${#failed[@]}
if [ "${PROBE_ONLY}" = 1 ]; then
    if [ "${ns}" -gt 0 ]; then
        echo "build-preflight: FAIL -- build-all cannot build $((ns)) of ${n} triples on this host (see SKIP lines). Do not report 'build-all: success' from here."
        exit 1
    fi
    echo "build-preflight: OK -- all ${n} triples compile and link a probe"; exit 0
fi
names() { local out=""; for x in "$@"; do out="${out:+${out}, }${x%%:*}"; done; printf '%s' "${out}"; }
# Positive assertion: every triple is accounted for exactly once.
if [ $((nb + ns + nf)) -ne "${n}" ]; then
    echo "build-all: INTERNAL ERROR -- ${n} triples, ${nb}+${ns}+${nf} accounted for"; exit 1
fi
if [ "${nf}" -gt 0 ]; then
    echo "build-all: FAIL -- ${nb}/${n} built, ${ns} skipped, ${nf} FAILED: $(names "${failed[@]}")"
    exit 1
fi
if [ "${ns}" -gt 0 ]; then
    msg="build-all: INCOMPLETE -- ${nb}/${n} built ($(names "${built[@]+"${built[@]}"}")); SKIPPED, not built: $(names "${skipped[@]}")"
    if [ "${ALLOW_SKIP}" = 1 ]; then echo "${msg} (BUILD_ALL_ALLOW_SKIP=1)"; exit 0; fi
    echo "${msg}"; exit 1
fi
echo "build-all: OK -- ${nb}/${n} triples built and verified: $(names "${built[@]}")"
exit 0
