#!/usr/bin/env bash
# check_examples.sh — the shipped examples/ compile.  Story 127.87.
#
# Before this, nothing in the Makefile or .github/ read examples/ at all, so
# the examples were outside every gate. The first symptom found was that
# examples/mortgage-web cannot be type-checked from a fresh clone: its
# interface files are gitignored (*.tki, .gitignore:7). They are build
# artifacts, so this gate GENERATES them the way a user must -- with
# --emit-interface, dependencies first -- in a private directory, and never
# reads a stale copy from the source tree.
#
# For every example:
#   1. each module is --check'ed in dependency order, emitting its interface
#      so the modules after it can import it (-I);
#   2. a single-module example is also built to a binary, so a link failure
#      is caught, not only a type error.
#
# Multi-module examples are linked too (127.76).  Until 127.76 they could not
# be: from a3adceb (2026-07-07) the driver joined the per-module .ll paths into
# ONE string and compile_binary passed it to clang as a single argv entry
# ("no such file or directory: '/tmp/tkc_proj_X/m0.ll /tmp/tkc_proj_X/m1.ll'"),
# so this gate skipped them behind EXAMPLES_LINK_MULTI=1.  That is now the
# default; EXAMPLES_LINK_MULTI=0 restores the old check-only behaviour.
# test/conform/C077 pins the driver fix itself.
#
# EVERY tracked examples/**/*.tk must appear in EXAMPLES below, so a new
# example cannot slip past the gate by not being listed.

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 1
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

# example-dir : modules in dependency order (imported before importer)
EXAMPLES=(
  "apihandler:main.tk"
  "bookmarks-api:model.tk api.tk main.tk"
  "datapipe:main.tk"
  "mortgage:model.tk calc.tk main.tk"
  "mortgage-web:pages/index.tk pages/calculate.tk pages/app.tk"
  "pagehandler:main.tk"
  "tkgrep:main.tk"
)

[ -x "${TKC}" ] || { echo "check-examples: no compiler at ${TKC} (run 'make')" >&2; exit 1; }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_examples.XXXXXX")" || exit 1
trap 'rm -rf "${WORK}"' EXIT

fail=0; checked=0; built=0

# ── completeness: every tracked example source is listed ─────────────────
listed="${WORK}/listed"; : > "${listed}"
for e in "${EXAMPLES[@]}"; do
  dir="${e%%:*}"
  for m in ${e#*:}; do echo "examples/${dir}/${m}" >> "${listed}"; done
done
while IFS= read -r f; do
  if ! grep -qxF "${f}" "${listed}"; then
    echo "FAIL ${f}: tracked example source not listed in scripts/check_examples.sh"
    fail=$((fail+1))
  fi
done < <(git ls-files 'examples/*.tk' 'examples/**/*.tk' | sort -u)

# ── compile ──────────────────────────────────────────────────────────────
for e in "${EXAMPLES[@]}"; do
  dir="${e%%:*}"; mods=(${e#*:})
  iface="${WORK}/${dir}/iface"; mkdir -p "${iface}"
  ok=1
  for m in "${mods[@]}"; do
    src="examples/${dir}/${m}"
    if [ ! -f "${src}" ]; then
      echo "FAIL ${src}: listed but missing"; fail=$((fail+1)); ok=0; continue
    fi
    cp "${src}" "${iface}/"
    base="$(basename "${m}")"
    log="${WORK}/${dir}/${base}.log"
    if (cd "${iface}" && "${TKC}" --check --emit-interface -I . "${base}") > "${log}" 2>&1; then
      checked=$((checked+1))
    else
      echo "FAIL ${src}: --check"
      grep '"severity":"error"' "${log}" | head -5 | sed 's/^/    /'
      fail=$((fail+1)); ok=0
    fi
  done
  [ "${ok}" -eq 1 ] || continue
  if [ "${#mods[@]}" -eq 1 ] || [ "${EXAMPLES_LINK_MULTI:-1}" = 1 ]; then
    bin="${WORK}/${dir}/bin"
    srcs=(); for m in "${mods[@]}"; do srcs+=("examples/${dir}/${m}"); done
    if "${TKC}" "${srcs[@]}" -o "${bin}" > "${WORK}/${dir}/build.log" 2>&1 && [ -x "${bin}" ]; then
      built=$((built+1))
    else
      echo "FAIL examples/${dir}: build"
      grep -v '^{' "${WORK}/${dir}/build.log" | head -5 | sed 's/^/    /'
      grep '"severity":"error"' "${WORK}/${dir}/build.log" | head -5 | sed 's/^/    /'
      fail=$((fail+1))
    fi
  fi
done

echo "check-examples: ${#EXAMPLES[@]} examples, ${checked} modules checked, ${built} built, ${fail} failure(s)"
[ "${fail}" -eq 0 ]
