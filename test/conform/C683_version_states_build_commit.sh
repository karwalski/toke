#!/usr/bin/env bash
# C683 -- `tkc --version` states the commit the binary was built from
# (131.33).
#
# Provenance stamps (tkc_sha) took the toke repo HEAD at RUN time, so a
# binary built commits ago was stamped with a commit it does not contain.
# make now writes src/build_info.h (TKC_GIT_SHA, TKC_GIT_DIRTY) and
# --version prints them:
#     toke <version> (git 1a2b3c4d5e6f)            clean checkout
#     toke <version> (git 1a2b3c4d5e6f, dirty)     src/, Makefile or VERSION modified
#     toke <version> (git unknown)                 not built in a git checkout
# and `--version --json` prints one object with git_sha / git_dirty.
#
# Cases 5-8 run the Makefile's generation rule into a scratch header: outside
# a checkout, inside ANOTHER repository (a tarball unpacked in some repo must
# not take its commit), a stated sha, and a sha that is not hex.
#
# Negative control: TKC=<437dc471's toke> -- cases 1-4 fail (it prints
# `toke 3.1.0` and rejects --json); cases 5-8 fail on its Makefile.
#
# Story: 131.33

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/toke}"
MK="${MK:-${REPO_ROOT}/Makefile}"
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
command -v python3 >/dev/null 2>&1 || { echo "SKIP: python3 not found"; exit 0; }
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
VER="$(tr -d '[:space:]' < "${REPO_ROOT}/VERSION")"

plain="$("${TKC}" --version 2>&1)"
shape=$(printf '%s\n' "${plain}" | grep -cE "^toke ${VER//./\\.} \\(git ([0-9a-f]{12}(, dirty)?|unknown)\\)\$")
expect "1 --version is one line: toke VERSION (git <sha>[, dirty] | unknown)" \
    "$(printf '%s\n' "${plain}" | wc -l | tr -d ' ') ${shape}" "1 1"

sha=$(printf '%s' "${plain}" | sed -n 's/.*(git \([0-9a-f]*\).*/\1/p')
case "${plain}" in
    *", dirty)")      dirty=true ;;
    *"(git unknown)") dirty=null ;;
    *)                dirty=false ;;
esac
[ -n "${sha}" ] || sha=null
js() {
    "${TKC}" "$@" 2>/dev/null | python3 2>/dev/null -c '
import json, sys
d = json.loads(sys.stdin.read())
print(d["name"], d["version"], d["git_sha"] if d["git_sha"] else "null",
      "null" if d["git_dirty"] is None else str(d["git_dirty"]).lower())'
}
expect "2 --version --json agrees with the plain line" "$(js --version --json)" "toke ${VER} ${sha} ${dirty}"
expect "3 --json --version is the same object" "$(js --json --version)" "toke ${VER} ${sha} ${dirty}"
"${TKC}" --json > "${WORK}/o" 2>&1; rc=$?
expect "4 --json without --version is still an unknown flag (rc 3)" "${rc}" "3"

# the generation rule, into a scratch header
gen() {   # gen DIR [VAR=VAL ...] -> "sha dirty" from the header make writes
    local d="$1"; shift
    ( cd "$d" && env -u TKC_GIT_SHA -u TKC_GIT_DIRTY make -s -f "${MK}" BUILD_INFO_H=b.h "$@" b.h >/dev/null 2>&1 )
    sed -n 's/#define TKC_GIT_SHA "\(.*\)"/\1/p; s/#define TKC_GIT_DIRTY \(.*\)/\1/p' "$d/b.h" 2>/dev/null | tr '\n' ' '
}
mkdir -p "${WORK}/plain" "${WORK}/other/sub"
expect "5 outside a git checkout: unknown, not dirty" "$(gen "${WORK}/plain")" "unknown 0 "
( cd "${WORK}/other" && git init -q && git -c user.email=t@t -c user.name=t commit -q --allow-empty -m x ) >/dev/null 2>&1
expect "6 inside another repository (not its top): unknown" "$(gen "${WORK}/other/sub")" "unknown 0 "
expect "7 a stated sha and dirty flag are written" \
    "$(gen "${WORK}/plain" TKC_GIT_SHA=0123456789ab TKC_GIT_DIRTY=1)" "0123456789ab 1 "
expect "8 a sha that is not hex is refused (unknown)" \
    "$(gen "${WORK}/plain" 'TKC_GIT_SHA=ab"; x' TKC_GIT_DIRTY=1)" "unknown 0 "

echo "C683: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
