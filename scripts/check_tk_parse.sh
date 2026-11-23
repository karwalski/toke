#!/usr/bin/env bash
# check_tk_parse.sh — every committed .tk outside test/ parses.  Story 131.142.
#
# 131.52 found five committed .tk files that no longer parsed: the GitHub
# Linguist samples (underscored identifiers, E1003; `=` for `==`, E2002) and
# a stale results/ copy of a toke-test-programs solution. Nothing compiled
# them, so the language moved and they were left behind. test/ is excluded
# because it holds deliberate negative inputs; its files are owned by the
# conformance and diagnostic suites.
#
# Every `git ls-files '*.tk'` path outside test/ must pass `tkc --dump-ast`
# (lex + parse only). The Linguist samples are what a Linguist submission
# would ship as toke, so they must also pass `tkc --check` with no output at
# all (no error and no warning).
#
# --selftest is the negative control: it runs the same parse step on a file
# with an underscored identifier and on one with `=` for `==` and fails
# unless both are rejected.

set -uo pipefail
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${REPO_ROOT}" || exit 1
TKC="${TKC:-${REPO_ROOT}/tkc}"
export TKC_STDLIB_DIR="${TKC_STDLIB_DIR:-${REPO_ROOT}/src/stdlib}"
SAMPLES="linguist-submission/samples/toke/"

[ -x "${TKC}" ] || { echo "check-tk-parse: no compiler at ${TKC} (run 'make')" >&2; exit 1; }
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_tk_parse.XXXXXX")" || exit 1
trap 'rm -rf "${WORK}"' EXIT

parses() { "${TKC}" --dump-ast "$1" > /dev/null 2> "${WORK}/err"; }

if [ "${1:-}" = "--selftest" ]; then
  printf 'm=a;\nf=bad_name():i64{<0};\n' > "${WORK}/neg1.tk"
  printf 'm=a;\nf=g(x:i64):i64{if(x=1){<1};<0};\n' > "${WORK}/neg2.tk"
  printf 'm=a;\nf=g(x:i64):i64{if(x==1){<1};<0};\n' > "${WORK}/pos.tk"
  rc=0
  for n in neg1 neg2; do
    if parses "${WORK}/${n}.tk"; then echo "check-tk-parse selftest: ${n} parsed, expected a rejection"; rc=1; fi
  done
  parses "${WORK}/pos.tk" || { echo "check-tk-parse selftest: the positive control did not parse"; rc=1; }
  [ "${rc}" -eq 0 ] && echo "check-tk-parse selftest: ok (2 rejected, 1 accepted)"
  exit "${rc}"
fi

total=0; fail=0
while IFS= read -r f; do
  case "${f}" in test/*) continue ;; esac
  total=$((total+1))
  if ! parses "${f}"; then
    fail=$((fail+1))
    echo "FAIL parse: ${f}: $(head -c 300 "${WORK}/err")"
    continue
  fi
  case "${f}" in
    "${SAMPLES}"*)
      out="$("${TKC}" --check "${f}" 2>&1)"; crc=$?
      if [ "${crc}" -ne 0 ] || [ -n "${out}" ]; then
        fail=$((fail+1))
        echo "FAIL check (Linguist sample must be clean): ${f}: $(printf '%s' "${out}" | head -c 300)"
      fi ;;
  esac
done < <(git ls-files -- '*.tk')

if [ "${fail}" -ne 0 ]; then
  echo "check-tk-parse: ${fail} of ${total} committed .tk files outside test/ fail"
  exit 1
fi
echo "check-tk-parse: ${total} committed .tk files outside test/ parse (Linguist samples check clean)"
