#!/usr/bin/env bash
# run_codegen.sh — C-series codegen cases with pinned expected results (123.6).
#
# Usage:  bash test/codegen/run_codegen.sh            (make test-codegen)
#         TKC=/path/to/tkc bash test/codegen/run_codegen.sh
#
# WHY THIS EXISTS BESIDE diff-codegen.  `make diff-codegen` also compiles and
# runs every test/codegen/*.tk, but it pins a hash of stdout and the exit
# code at -O2 and nothing else.  That cannot see three things these cases
# are about:
#   * WHICH trap fired.  RT003/RT004/RT005 all exit 1 with nothing on stdout;
#     only stderr says which, and diff-codegen discards stderr.
#   * -O0.  A check the optimiser drops, or UB the optimiser exploits, shows
#     up as -O0 and -O2 disagreeing.  Every case is built at both.
#   * multi-module programs.  diff-codegen builds each .tk alone, so a program
#     that imports a sibling module cannot be expressed there (see multimod/).
#
# CASE FORMAT
#   <name>.tk         single-file program with f=main
#   <name>.expected   exact expected stdout
#   header comment    `expect-exit: N`     (default 0)
#                     `expect-stderr: TEXT` (some stderr line must START with TEXT)
#   multimod/<case>/  a multi-module program: every *.tkm in the directory is
#                     passed to one tkc invocation (sorted, so name the files
#                     in dependency order); expected output in
#                     multimod/<case>/expected, header lines in main.tkm.
#                     The files are .tkm, not .tk, precisely so that
#                     diff-codegen's single-file glob does not pick them up.
#
# KNOWN DEFECTS
#   A case whose path is listed in test/codegen-quarantine.txt is a WITNESS for
#   a filed defect: its .expected holds the CORRECT output, never the buggy
#   one.  It is reported XFAIL while it disagrees, and it FAILS this runner the
#   moment it passes (XPASS), so the quarantine line has to be removed — the
#   same self-failing contract diff-codegen applies.  Multi-module witnesses
#   are quarantined by their directory path.
#
# Exit status: 0 when every case passes or is a known XFAIL; 1 otherwise.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
QUAR="${REPO_ROOT}/test/codegen-quarantine.txt"
OPTS="${CODEGEN_OPTS:--O0 -O2}"

if [ ! -x "${TKC}" ]; then
    echo "run_codegen: tkc not found at ${TKC} (run 'make' first)"
    exit 1
fi

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_codegen_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

PASS=0; FAIL=0; XFAIL=0
FAILED=()

quarantined() {
    [ -f "${QUAR}" ] || return 1
    awk -v k="$1" '!/^[[:space:]]*#/ { sub(/#.*/, ""); gsub(/[[:space:]]/, ""); if ($0 == k) f=1 } END { exit f ? 0 : 1 }' "${QUAR}"
}

header() {  # header FILE KEY -> value of the first `KEY: value` line
    awk -v k="$1" 'BEGIN{k=k":"} { i=index($0,k); if (i) { v=substr($0,i+length(k)); sub(/^[ \t]+/,"",v); sub(/[ \t]*(\*\))?[ \t]*$/,"",v); print v; exit } }' "$2"
}

# check NAME REL MAINFILE EXPECTED_FILE SRC...
check() {
    local name="$1" rel="$2" mainf="$3" expf="$4"; shift 4
    local want_exit want_err
    want_exit="$(header expect-exit "${mainf}")"; want_exit="${want_exit:-0}"
    want_err="$(header expect-stderr "${mainf}")"
    local problems=() o
    for o in ${OPTS}; do
        local bin="${WORK}/bin" rc=0
        rm -f "${bin}"
        if ! "${TKC}" --allow-all "${o}" --out "${bin}" "$@" > "${WORK}/build.log" 2>&1 || [ ! -x "${bin}" ]; then
            problems+=("${o}: did not build: $(awk '/"severity":"error"/' "${WORK}/build.log" | head -1 | cut -c1-220)")
            continue
        fi
        # The subshell keeps bash's own "Segmentation fault" notice out of the log.
        # (the trailing `exit` stops bash exec-ing the binary in place of the
        # subshell, which would move the notice back to this shell's stderr).
        ( "${bin}" < /dev/null > "${WORK}/out" 2> "${WORK}/err"; exit $? ) 2>/dev/null || rc=$?
        if [ "${rc}" != "${want_exit}" ]; then
            if [ "${rc}" -gt 128 ]; then
                problems+=("${o}: killed by signal $((rc - 128)), want exit ${want_exit}")
            else
                problems+=("${o}: exit ${rc}, want ${want_exit}")
            fi
        fi
        if ! cmp -s "${WORK}/out" "${expf}"; then
            problems+=("${o}: stdout differs from ${expf#"${REPO_ROOT}/"}:")
            while IFS= read -r l; do problems+=("    ${l}"); done \
                < <(diff "${expf}" "${WORK}/out" | head -12)
        fi
        if [ -n "${want_err}" ] && ! awk -v p="${want_err}" 'index($0,p)==1{f=1} END{exit !f}' "${WORK}/err"; then
            problems+=("${o}: no stderr line starts with '${want_err}'; got: $(head -2 "${WORK}/err" | tr '\n' '|')")
        fi
    done
    if quarantined "${rel}"; then
        if [ ${#problems[@]} -eq 0 ]; then
            echo "FAIL ${name}: XPASS — passes but is quarantined; remove its line from test/codegen-quarantine.txt"
            FAIL=$((FAIL + 1)); FAILED+=("${name}")
        else
            echo "XFAIL ${name} (known defect, see test/codegen-quarantine.txt)"
            XFAIL=$((XFAIL + 1))
        fi
        return
    fi
    if [ ${#problems[@]} -eq 0 ]; then
        echo "PASS ${name}"
        PASS=$((PASS + 1))
    else
        echo "FAIL ${name}"
        for p in "${problems[@]}"; do echo "      ${p}"; done
        FAIL=$((FAIL + 1)); FAILED+=("${name}")
    fi
}

echo "codegen cases (${OPTS}) — ${TKC}"
for tk in "${SCRIPT_DIR}"/*.tk; do
    [ -e "${tk}" ] || continue
    name="$(basename "${tk}" .tk)"
    exp="${SCRIPT_DIR}/${name}.expected"
    if [ ! -f "${exp}" ]; then
        echo "FAIL ${name}: no ${name}.expected"; FAIL=$((FAIL + 1)); FAILED+=("${name}"); continue
    fi
    check "${name}" "test/codegen/${name}.tk" "${tk}" "${exp}" "${tk}"
done
for d in "${SCRIPT_DIR}"/multimod/*/; do
    [ -d "${d}" ] || continue
    d="${d%/}"; case_name="multimod/$(basename "${d}")"
    srcs=(); while IFS= read -r f; do srcs+=("${f}"); done < <(ls "${d}"/*.tkm | sort)
    check "${case_name}" "test/codegen/${case_name}" "${d}/main.tkm" "${d}/expected" "${srcs[@]}"
done

echo "codegen: ${PASS} passed, ${FAIL} failed, ${XFAIL} known-defect (quarantined)"
if [ ${FAIL} -gt 0 ]; then
    printf '  failed: %s\n' "${FAILED[@]}"
    exit 1
fi
exit 0
