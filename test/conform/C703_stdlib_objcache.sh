#!/usr/bin/env bash
# C703 -- conform harnesses link cached stdlib objects, and the cache is never
# stale (127.270).
#
# 127.268 made the hand-built C harnesses link the stdlib closure tkc itself
# links (scripts/stdlib_link_set.sh), which ended hand lists breaking at
# merge -- and made every harness compile that closure, mostly under ASan, on
# every run: 43 timed scripts went from 508 s to 1,794 s.
# `stdlib_link_set.sh --objects ... -- CFLAGS` prints prebuilt objects instead,
# from a cache keyed by compiler, flags, source bytes and the bytes of every
# header the source includes (scripts/stdlib_objcache.py).  A cache that
# saves time by linking a stale object would be worse than none, so this
# script drives it on a private copy of the stdlib tree:
#
#   1  cold: one object per source of the plain set, in the same order, all
#      built; the default cache for a non-repo tree is <tree>/.tk_objcache
#   2  warm: the same paths, every one a hit, nothing built
#   3  touching a source (mtime only, bytes unchanged) rebuilds nothing
#   4  editing one .c rebuilds that object only; every other path is unchanged
#   5  editing a header rebuilds exactly the sources whose compile includes
#      it (the set `cc -M` names), nothing else
#   6  another flag set is another object set; the first still hits
#   7  another compiler (same flags) is another object set
#   8  four cold runs at once agree, publish complete objects that link,
#      and leave no temp entries behind
#   9  a source that does not compile: non-zero, the error on stderr, nothing
#      on stdout, nothing published
#  10  TKC_STDLIB_OBJCACHE=off prints the sources, exactly the plain set
#  11  misuse fails loudly: --objects without `--`, flags without --objects
#  12  every stdlib_link_set.sh call in test/conform uses --objects, outside
#      the commented allowlist (calls whose flags change every run)
#
# Negative control: on main ce75d43c the --objects mode does not exist, so
# cases 1-11 fail; case 12 lists the 47 converted harnesses.
#
# Story: 127.270

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
LS="${C703_LINKSET:-${REPO_ROOT}/scripts/stdlib_link_set.sh}"
CC="${CC:-cc}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "CONFORM-SKIP: python3 not found (the cache needs it)"; exit 0; }
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c703_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
WORK="$(cd "${WORK}" && pwd -P)"   # tkc prints physical paths (/private/var/...)
T="${WORK}/stdlib"
cp -R "${REPO_ROOT}/src/stdlib" "${T}" || { echo "FAIL: cannot copy the stdlib tree"; exit 1; }
rm -rf "${T}/.tk_objcache"
CF=(-std=c99 -D_GNU_SOURCE -O0 -iquote "${T}" -I"${T}")
unset TKC_STDLIB_OBJCACHE

# objs OUT LOG [ENV...] -- the plain base set as objects
objs() {
    local out="$1" log="$2"; shift 2
    : > "${log}"
    env "$@" bash "${LS}" --tkc "${TKC}" --stdlib "${T}" --cc "${CC}" --objects --objlog "${log}" \
        -- "${CF[@]}" > "${out}" 2> "${out}.err"
}
built() { awk '$1 == "built" { print $2 }' "$1" | sort; }
nbuilt() { awk '$1 == "built"' "$1" | wc -l | tr -d ' '; }
nhit() { awk '$1 == "hit"' "$1" | wc -l | tr -d ' '; }

bash "${LS}" --tkc "${TKC}" --stdlib "${T}" > "${WORK}/srcs" 2>/dev/null
N="$(wc -l < "${WORK}/srcs" | tr -d ' ')"

# 1: cold
objs "${WORK}/o1" "${WORK}/l1"; r=$?
stems() { sed 's|.*/||; s|\.[co]$||' "$1"; }
expect "1 cold: one object per source, same order, all ${N} built, cache in <tree>/.tk_objcache" \
    "${r} $(wc -l < "${WORK}/o1" | tr -d ' ') $(stems "${WORK}/srcs" | cmp -s - <(stems "${WORK}/o1") && echo same-order) $(nbuilt "${WORK}/l1") $(grep -c "^${T}/.tk_objcache/o/" "${WORK}/o1")" \
    "0 ${N} same-order ${N} ${N}"

# 2: warm
objs "${WORK}/o2" "${WORK}/l2"
expect "2 warm: same paths, ${N} hits, 0 built" \
    "$([ -s "${WORK}/o2" ] && cmp -s "${WORK}/o1" "${WORK}/o2" && echo same) $(nhit "${WORK}/l2") $(nbuilt "${WORK}/l2")" "same ${N} 0"

# 3: touch only
sleep 1; touch "${T}/str.c" "${T}/tk_runtime.h"
objs "${WORK}/o3" "${WORK}/l3"
expect "3 touching str.c and tk_runtime.h (bytes unchanged) rebuilds nothing" \
    "$([ -s "${WORK}/o3" ] && cmp -s "${WORK}/o1" "${WORK}/o3" && echo same) $(nbuilt "${WORK}/l3")" "same 0"

# 4: edit one .c
printf '\nint c703_probe_edit_marker = 1;\n' >> "${T}/args.c"
objs "${WORK}/o4" "${WORK}/l4"
expect "4 editing args.c rebuilds args.c only; only its path changes" \
    "$(built "${WORK}/l4") | $(diff "${WORK}/o1" "${WORK}/o4" | grep -c '^>') $(diff "${WORK}/o1" "${WORK}/o4" | grep '^>' | grep -c '/args\.o$')" \
    "${T}/args.c | 1 1"

# 5: edit a header -> exactly its includers
H="${T}/encoding.h"
while IFS= read -r f; do
    "${CC}" "${CF[@]}" -M "${f}" 2>/dev/null | tr ' \\' '\n\n' | grep -qxF "${H}" && echo "${f}"
done < "${WORK}/srcs" | sort > "${WORK}/want5"
printf '\n/* c703: a header edit */\n#define C703_HEADER_EDIT 1\n' >> "${H}"
objs "${WORK}/o5" "${WORK}/l5"
built "${WORK}/l5" > "${WORK}/got5"
expect "5 editing encoding.h rebuilds exactly the $(wc -l < "${WORK}/want5" | tr -d ' ') sources cc -M says include it (of ${N})" \
    "$( [ -s "${WORK}/want5" ] && cmp -s "${WORK}/want5" "${WORK}/got5" && echo exact || { echo "want:"; cat "${WORK}/want5"; echo "got:"; cat "${WORK}/got5"; })" "exact"

# 6: another flag set
CF0=("${CF[@]}"); CF=("${CF0[@]}" -DC703_OTHER_FLAGS=1)
objs "${WORK}/o6" "${WORK}/l6"; CF=("${CF0[@]}")
objs "${WORK}/o6b" "${WORK}/l6b"
expect "6 adding -DC703_OTHER_FLAGS builds all ${N} anew (no path shared); the old flags still all hit" \
    "$(nbuilt "${WORK}/l6") $(sort "${WORK}/o5" "${WORK}/o6" | uniq -d | wc -l | tr -d ' ') $(nhit "${WORK}/l6b")" "${N} 0 ${N}"

# 7: another compiler
mkdir -p "${WORK}/bin"
printf '#!/bin/sh\nexec %s "$@"\n' "$(command -v "${CC}")" > "${WORK}/bin/c703cc"; chmod +x "${WORK}/bin/c703cc"
CC0="${CC}"; CC="${WORK}/bin/c703cc"
objs "${WORK}/o7" "${WORK}/l7"; CC="${CC0}"
expect "7 another compiler (same flags) builds all ${N} anew" "$(nbuilt "${WORK}/l7")" "${N}"

# 8: four cold runs at once
CF=("${CF0[@]}" -DC703_RACE=1)
for i in 1 2 3 4; do objs "${WORK}/o8.${i}" "${WORK}/l8.${i}" & done; wait
CF=("${CF0[@]}")
agree=yes; for i in 2 3 4; do cmp -s "${WORK}/o8.1" "${WORK}/o8.${i}" || agree=no; done
printf 'int main(void){return 0;}\n' > "${WORK}/m.c"
LINK=(); while IFS= read -r l; do LINK+=("$l"); done < "${WORK}/o8.1"
"${CC}" "${CF[@]}" -o "${WORK}/m" "${WORK}/m.c" ${LINK[@]+"${LINK[@]}"} -lm -lz -lpthread > "${WORK}/m.log" 2>&1 && "${WORK}/m" && linked=links || linked="no: $(head -3 "${WORK}/m.log")"
expect "8 four concurrent cold runs: same ${N} paths, they link, no temp entry left" \
    "${agree} $(wc -l < "${WORK}/o8.1" | tr -d ' ') ${linked} $(ls "${T}/.tk_objcache/tmp" | wc -l | tr -d ' ')" "yes ${N} links 0"

# 9: a compile error
nobj="$(ls "${T}/.tk_objcache/o" | wc -l | tr -d ' ')"
printf '\nthis is not C;\n' >> "${T}/collections.c"
objs "${WORK}/o9" "${WORK}/l9"; r=$?
expect "9 a source that does not compile: non-zero, compiler error on stderr, stdout empty, nothing published" \
    "$([ "${r}" -ne 0 ] && echo nonzero) $(grep -c 'collections.c' "${WORK}/o9.err" | awk '{print ($1 > 0) ? "named" : "unnamed"}') $(wc -c < "${WORK}/o9" | tr -d ' ') $(( $(ls "${T}/.tk_objcache/o" | wc -l) - nobj )) $(ls "${T}/.tk_objcache/tmp" | wc -l | tr -d ' ')" \
    "nonzero named 0 0 0"

# 10: off
objs "${WORK}/o10" "${WORK}/l10" TKC_STDLIB_OBJCACHE=off
expect "10 TKC_STDLIB_OBJCACHE=off prints the plain source set" \
    "$(cmp -s "${WORK}/srcs" "${WORK}/o10" && echo same)" "same"

# 11: misuse
bash "${LS}" --tkc "${TKC}" --stdlib "${T}" --objects > "${WORK}/o11a" 2> "${WORK}/e11a"; r1=$?
bash "${LS}" --tkc "${TKC}" --stdlib "${T}" -- -O0 > "${WORK}/o11b" 2> "${WORK}/e11b"; r2=$?
expect "11 --objects without flags, and flags without --objects: non-zero, nothing on stdout, a reason" \
    "${r1} $(wc -c < "${WORK}/o11a" | tr -d ' ') $(grep -c 'needs the compile flags' "${WORK}/e11a") ${r2} $(wc -c < "${WORK}/o11b" | tr -d ' ') $(grep -c 'need --objects' "${WORK}/e11b")" \
    "2 0 1 2 0 1"

# 12: harnesses use --objects
# Allowlist: script -> why its call stays on sources.
cat > "${WORK}/allow" <<'ALLOW'
C2031_err_box_fast_path.sh	builds the same sources once per sanitizer flag set (plain, ASan+UBSan) for one test driver
C208_stdlib_residue_135_3j.sh	the llmtool driver force-includes a per-run counter header (-include $WORK/cnt.h)
C276_llmtool_args_escapes.sh	force-includes a per-run counter header (-include $WORK/cnt.h)
C342_llmtool_args_nested.sh	force-includes a per-run counter header (-include $WORK/cnt.h)
C591_llm_inference_hardening.sh	the mlx build bakes in a per-run port (-DMLX_BRIDGE_PORT)
C696_harness_stdlib_link_set.sh	pins the sources mode itself
C703_stdlib_objcache.sh	this file
C704_diag_utf8_cut_db_time_summary.sh	reads the db set's source order; links nothing
T004_stdlib_link_set.sh	pins the sources mode itself
ALLOW
python3 - "${REPO_ROOT}/test/conform" "${WORK}/allow" > "${WORK}/srcmode" <<'PY'
import glob, os, sys
d, allow = sys.argv[1], {l.split('\t')[0] for l in open(sys.argv[2]) if l.strip()}
for f in sorted(glob.glob(os.path.join(d, '*.sh'))):
    lines = open(f, errors='replace').read().split('\n')
    i = 0
    while i < len(lines):
        start, buf = i, lines[i]
        while buf.endswith('\\') and i + 1 < len(lines):
            i += 1; buf = buf[:-1] + ' ' + lines[i].strip()
        if 'stdlib_link_set.sh' in buf and not buf.lstrip().startswith('#') \
                and '--objects' not in buf and os.path.basename(f) not in allow:
            print('%s:%d' % (os.path.basename(f), start + 1))
        i += 1
PY
expect "12 every stdlib_link_set.sh call in test/conform links --objects (outside the allowlist)" \
    "$(cat "${WORK}/srcmode")" ""

echo "C703: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
