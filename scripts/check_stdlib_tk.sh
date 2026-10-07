#!/usr/bin/env bash
# check_stdlib_tk.sh — the stdlib's own toke sources type-check.  Story 127.126.
#
# stdlib/*.tk holds two kinds of file: a handful of genuine toke modules
# (they declare `m=`), and one-to-twenty-line stubs whose whole body is a
# comment. No target compiled either kind, which is how two of the genuine
# ones came to be broken without anyone noticing.
#
# Every stdlib/*.tk with an `m=` declaration is --check'ed. A file that fails
# must be listed in KNOWN_RED below with its story; the list is SELF-FAILING:
# an entry whose file now passes is stale and fails the gate, so the list can
# only shrink and cannot shrink silently. A comment-only stub is skipped, and
# counted, so a stub that grows code is picked up automatically.
#
# 136.68: test/stdlib/*.tk -- toke programs that exercise a stdlib module --
# are type-checked here too. 22 of 37 had stopped passing --check with nobody
# noticing, because nothing compiled them; 21 were archived
# (~/tk/archive/toke-test-stdlib-tk-20260926/) and every one that remains must
# check. They take the same KNOWN_RED list (empty for them today).

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 1
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

# file : story -- reason.  Remove the line in the commit that fixes the file.
# Empty since 127.199/135.14 (2026-10-07): stdlib/toon.tk, its one entry, was
# withdrawn with the other dead parallel stdlib .tk files and archived.
KNOWN_RED=()

[ -x "${TKC}" ] || { echo "check-stdlib-tk: no compiler at ${TKC} (run 'make')" >&2; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_stdlib_tk.XXXXXX")" || exit 1
trap 'rm -rf "${WORK}"' EXIT

known_story() {
  local f="$1" e
  for e in ${KNOWN_RED[@]+"${KNOWN_RED[@]}"}; do
    [ "${e%%:*}" = "${f}" ] && { echo "${e#*:}"; return 0; }
  done
  return 1
}

fail=0; ok=0; red=0; stubs=0; seen=""; tests=0
for f in stdlib/*.tk test/stdlib/*.tk; do
  [ -e "${f}" ] || continue
  case "${f}" in test/*) tests=$((tests+1)) ;; esac
  if ! grep -q '^m=' "${f}"; then stubs=$((stubs+1)); continue; fi
  seen="${seen} ${f}"
  log="${WORK}/$(basename "${f}").log"
  if "${TKC}" --check "${f}" > "${log}" 2>&1; then
    if why="$(known_story "${f}")"; then
      echo "FAIL ${f}: passes now, but is listed in KNOWN_RED -- delete its entry (${why%% -- *})"
      fail=$((fail+1))
    else
      ok=$((ok+1))
    fi
  else
    if why="$(known_story "${f}")"; then
      echo "known-red ${f} [${why%% -- *}]"
      red=$((red+1))
    else
      echo "FAIL ${f}: --check"
      grep '"severity":"error"' "${log}" | head -5 | sed 's/^/    /'
      fail=$((fail+1))
    fi
  fi
done
for e in ${KNOWN_RED[@]+"${KNOWN_RED[@]}"}; do
  f="${e%%:*}"
  case " ${seen} " in *" ${f} "*) ;; *)
    echo "FAIL ${f}: listed in KNOWN_RED but not a checked stdlib source -- delete its entry"
    fail=$((fail+1)) ;;
  esac
done

# A glob that silently matched nothing would read as a clean pass (132.68).
if [ "${tests}" -lt 10 ]; then
  echo "FAIL test/stdlib/*.tk: only ${tests} found -- the glob no longer reaches the test programs"
  fail=$((fail+1))
fi
echo "check-stdlib-tk: ${ok} clean (incl. ${tests} test/stdlib programs), ${red} known-red, ${stubs} comment-only stubs skipped, ${fail} failure(s)"
[ "${fail}" -eq 0 ]
