#!/usr/bin/env bash
# stdlib_link_set.sh — the stdlib C sources (or link flags) a hand-built C
# harness needs for a set of std modules (story 127.268).
#
# usage: scripts/stdlib_link_set.sh [--stdlib DIR] [--tkc PATH] [--flags]
#                                    [--except FILE]... [MODULE...]
#
#   MODULE   a std module name as a program imports it: http, toml, crypto ...
#            None: the base set every program links (runtime, capabilities,
#            and the modules the runtime itself calls).
#   --stdlib the stdlib tree to resolve against (default: $TKC_STDLIB_DIR,
#            else <repo>/src/stdlib) — a harness's negative-control override
#            passes its own tree here
#   --tkc    the compiler (default: $TKC, else <repo>/tkc)
#   --flags  print the modules' extra link flags (-lssl, -lz ...) instead
#   --except leave FILE (a path relative to the stdlib tree, e.g. http.c) out
#            of the set: the harness supplies it itself — #included into the
#            test file, or compiled separately with fault-injection macros
#
# Prints one absolute path (or flag) per line; exits non-zero, with the
# reason on stderr, if any module cannot be resolved.
#
# WHY.  Conform harnesses that compile a C test file against stdlib C cores
# used to name every core by hand.  A stdlib dependency change (121.74: toml.c
# now calls file_read, so toml needs file.c) then broke each harness that had
# listed toml.c without file.c — at merge, in a script nobody had touched.
# The dependency table already exists: src/stdlib_deps.c, which `tkc --out`
# links from and `tkc --emit-deps` prints.  This script asks tkc for it, on a
# stub program importing the requested modules, so a harness's link set is
# the same closure a real toke program importing those modules is linked with
# (runtime, capabilities, the module cores, their glue, their dependencies and
# vendored sources).  One table, no copies.
#
# A harness uses it like this (bash 3.2 safe, keeps paths with spaces whole):
#
#   LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" http)" \
#       || { echo "FAIL: stdlib link set: ${LINKSET}"; exit 1; }
#   LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
#   cc ... test.c "${LINK[@]}" ...
#
# Story: 127.268
set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
STDLIB="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
TKC_BIN="${TKC:-${REPO_ROOT}/tkc}"
WANT=sources
MODS=()
EXCEPT=()

while [ $# -gt 0 ]; do
    case "$1" in
        --stdlib) STDLIB="${2:?--stdlib needs a directory}"; shift 2 ;;
        --tkc)    TKC_BIN="${2:?--tkc needs a path}"; shift 2 ;;
        --flags)  WANT=flags; shift ;;
        --except) EXCEPT+=("${2:?--except needs a file}"); shift 2 ;;
        -h|--help) sed -n '2,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        -*) echo "stdlib_link_set: unknown option $1" >&2; exit 2 ;;
        *)
            case "$1" in
                *[!a-z0-9]*|'') echo "stdlib_link_set: '$1' is not a std module name" >&2; exit 2 ;;
            esac
            MODS+=("$1"); shift ;;
    esac
done
[ -x "${TKC_BIN}" ] || { echo "stdlib_link_set: tkc not found at ${TKC_BIN} (run make)" >&2; exit 1; }
[ -f "${STDLIB}/tk_runtime.c" ] || { echo "stdlib_link_set: ${STDLIB} is not a stdlib tree" >&2; exit 1; }
STDLIB="$(cd "${STDLIB}" && pwd)"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tk_linkset_XXXXXX")" || exit 1
trap 'rm -rf "${WORK}"' EXIT

# Aliases are letters only (lexer Profile-1); one per module, in order.
LETTERS=abcdefghijklmnopqrstuvwxyz
{
    echo "m=linkset;"
    i=0
    for m in ${MODS[@]+"${MODS[@]}"}; do
        a="z${LETTERS:$((i / 26 % 26)):1}${LETTERS:$((i % 26)):1}"
        echo "i=${a}:std.${m};"
        i=$((i + 1))
    done
    echo "f=main():i64{<0};"
} > "${WORK}/linkset.tk"

if ! TKC_STDLIB_DIR="${STDLIB}" "${TKC_BIN}" --emit-deps "${WORK}/linkset.tk" \
        > "${WORK}/deps" 2> "${WORK}/err"; then
    echo "stdlib_link_set: tkc --emit-deps failed for: ${MODS[*]-(base set)}" >&2
    head -5 "${WORK}/err" >&2
    exit 1
fi

# --emit-deps prints sources, then "---" and flags, then (for user modules
# only, never here) "---" and .ll files.
if [ "${WANT}" = sources ]; then
    awk '/^---$/ { exit } NF { print }' "${WORK}/deps" > "${WORK}/all"
    : > "${WORK}/out"
    for x in ${EXCEPT[@]+"${EXCEPT[@]}"}; do
        grep -qxF "${STDLIB}/${x}" "${WORK}/all" \
            || { echo "stdlib_link_set: --except ${x}: not in the link set" >&2; exit 1; }
    done
    while IFS= read -r f; do
        skip=0
        for x in ${EXCEPT[@]+"${EXCEPT[@]}"}; do
            [ "${f}" = "${STDLIB}/${x}" ] && skip=1
        done
        [ "${skip}" = 1 ] || echo "${f}" >> "${WORK}/out"
    done < "${WORK}/all"
    # Every printed source must exist: a missing file is a broken link set,
    # better named here than as a clang "no such file" deep in a harness.
    while IFS= read -r f; do
        [ -f "${f}" ] || { echo "stdlib_link_set: listed source missing: ${f}" >&2; exit 1; }
    done < "${WORK}/out"
    [ -s "${WORK}/out" ] || { echo "stdlib_link_set: empty link set" >&2; exit 1; }
else
    awk 'seen == 1 && /^---$/ { exit } seen == 1 && NF { print } /^---$/ { seen = 1 }' \
        "${WORK}/deps" > "${WORK}/out"
fi
cat "${WORK}/out"
