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

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 1
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"

# file : story -- reason.  Remove the line in the commit that fixes the file.
KNOWN_RED=(
  "stdlib/toon.tk:127.126 -- E1003 from line 209: the v0.3 migration (a6fe9cd) un-escaped its string escapes, leaving raw newlines, tabs and CRs inside literals and bare backslashes before quotes; it also still uses = for equality. Unused draft (std.toon is toon.c); fix or withdraw."
  "stdlib/path.tk:127.126 -- E4031 at lines 71 and 97, expected str got func: a let named base shadows the module function base(), and a bare use of the local is typed as the function. Compiler defect (reproduces at 04f6199, so not 127.164); the source is correct."
)

[ -x "${TKC}" ] || { echo "check-stdlib-tk: no compiler at ${TKC} (run 'make')" >&2; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_stdlib_tk.XXXXXX")" || exit 1
trap 'rm -rf "${WORK}"' EXIT

known_story() {
  local f="$1" e
  for e in "${KNOWN_RED[@]}"; do
    [ "${e%%:*}" = "${f}" ] && { echo "${e#*:}"; return 0; }
  done
  return 1
}

fail=0; ok=0; red=0; stubs=0; seen=""
for f in stdlib/*.tk; do
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
for e in "${KNOWN_RED[@]}"; do
  f="${e%%:*}"
  case " ${seen} " in *" ${f} "*) ;; *)
    echo "FAIL ${f}: listed in KNOWN_RED but not a checked stdlib source -- delete its entry"
    fail=$((fail+1)) ;;
  esac
done

echo "check-stdlib-tk: ${ok} clean, ${red} known-red, ${stubs} comment-only stubs skipped, ${fail} failure(s)"
[ "${fail}" -eq 0 ]
