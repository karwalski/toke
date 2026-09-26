#!/usr/bin/env bash
# C140_pkg_lock_pins.sh — pkg.lock pins commits and content hashes, and the
# package-name grammar is enforced (story 122.10).
#
# THE DEFECT.  ADR-0004 called the lock file "a convenience (recording commit
# hashes), not a correctness requirement", and pkg_lock_write did not even do
# that: it wrote `name = "x.y.z"` and nothing else, so a moved git tag or a
# tampered cache would have been fetched and built without complaint once
# fetch exists.  Names were written unquoted as TOML keys with no grammar
# check, so a dependency named `web"\n[commit]\nweb` wrote its own lines.
# ADR-0004 now carries a "Supply-chain security" section (design before any
# network code); this suite covers the parts that need no network.
#
# WHAT THIS DRIVES: test/test_pkg.c, built with -fsanitize=address,undefined
# against src/pkg.c --
#   lock format 2 round-trips [commit] and [hash];
#   commit = exactly 40/64 lower-case hex, not the null id (branch names,
#   tags, abbreviations refused); hash = sha256:<64 hex>;
#   pkg_lock_check_pinned refuses an entry with no commit or no hash;
#   pkg_lock_verify refuses a moved tag (commit) and tampered content (hash);
#   the reader refuses pins for unresolved names, duplicates, malformed pins
#   and an unknown lock-version, and still reads a pre-122.10 lock;
#   the writer refuses an injecting name and writes nothing;
#   manifests refuse names outside [a-z][a-z0-9]* and the reserved names.
# src/pkg.c is not linked into tkc (src/pkg.h), so no compiler behaviour
# changes; `make test-pkg` runs the same file without sanitizers.
#
# Negative control: make pkg_commit_valid accept any length -> "abbreviated
# commit refused", "41 digits refused" and the reader's "abbreviated commit"
# fail (126/129); drop the lock_find check
# for [commit]/[hash] -> "pin for an unresolved package" fails (128/129).
#
# Story: 122.10

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c140_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C140: pkg.lock commit + hash pins"
echo "---------------------------------"

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
if ! "${CC}" -std=c99 -D_GNU_SOURCE -Wall -Wextra -Wpedantic -Werror -g -O1 ${SAN} \
        -iquote "${REPO_ROOT}/src" -o "${WORK}/t" \
        "${REPO_ROOT}/test/test_pkg.c" "${REPO_ROOT}/src/pkg.c" >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 800 "${WORK}/cc.log")"
    exit 1
fi
echo "  PASS: built with ${SAN}"

TMPDIR="${WORK}" ASAN_OPTIONS="detect_leaks=${C140_LEAKS:-0}:abort_on_error=0" \
    "${WORK}/t" >"${WORK}/run.log" 2>&1
rc=$?
sed -n '/^FAIL /p' "${WORK}/run.log" | sed 's/^/  /'
line="$(sed -n 's/^\([0-9][0-9]*\) \/ \([0-9][0-9]*\) tests passed$/\1 \2/p' "${WORK}/run.log")"
if [ -z "${line}" ]; then
    echo "  FAIL: no summary line (exit ${rc}): $(tail -c 600 "${WORK}/run.log")"
    echo; echo "Results: 0 passed, 1 failed"; exit 1
fi
set -- ${line}
# the 122.10 tests must actually have run: 64 predate them
if [ "$2" -lt 100 ]; then
    echo "  FAIL: only $2 checks ran; the 122.10 checks are missing"
    echo; echo "Results: $1 passed, 1 failed"; exit 1
fi
if [ "${rc}" -ne 0 ] || [ "$1" -ne "$2" ]; then
    f=$(( $2 - $1 )); [ "${f}" -gt 0 ] || f=1
    echo; echo "Results: $1 passed, ${f} failed (exit ${rc})"; exit 1
fi
echo "  PASS: test_pkg $1 / $2"
echo; echo "Results: $1 passed, 0 failed"
exit 0
