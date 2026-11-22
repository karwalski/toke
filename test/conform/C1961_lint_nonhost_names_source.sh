#!/usr/bin/env bash
# C1961_lint_nonhost_names_source.sh — `make lint-nonhost` names the file of
# every failure, runs under the same compiler as `make lint`, and honours
# host-scoped nonhost-uncoverable skips (story 127.450).
#
# THE DEFECT.  CI's lint-nonhost failed with
#     FAIL:  NONHOST: cc1: all warnings being treated as errors
# and no file.  It ran under the default cc -- GCC on the Linux runner --
# while check_stdlib_analysis.py's header flags are clang's
# (-Wno-empty-translation-unit), so GCC failed the macro-only headers
# src/build_info.h and src/version.h with "x.h:17: error: ISO C forbids an
# empty translation unit".  That line has no column, DIAG_RE required one,
# and the fallback reported the output's last line.  Separately, the
# nonhost-uncoverable skips for log.c and sys_glue.c hold on macOS only;
# on Linux they build clean and were reported STALE.
#
# WHAT THIS DRIVES (no real compile: a stand-in CC prints GCC's text).
#   1  a column-less error is reported with its file and line;
#   2  an error with no location at all is reported with the source name;
#   3  `make -n lint-nonhost` runs the script under LINT_CC;
#   4  a nonhost-uncoverable@<other host> entry is neither in play nor an
#      error; @<this host> is in play; a bad qualifier is an error.
#
# Negative control: the script and Makefile at 58b1cc17 -- 1, 2 and 3 fail
# (the file is missing from the FAIL line; lint-nonhost has no CC=), and 4
# fails (the @host kind is "unknown record kind").
#
# Story: 127.450

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
PASS=0; FAIL=0
ok()  { echo "PASS $1"; PASS=$((PASS + 1)); }
bad() { echo "FAIL $1"; [ -n "${2:-}" ] && echo "        $2"; FAIL=$((FAIL + 1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1961_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

# A stand-in compiler: fails every source with GCC's empty-TU text (case 1)
# or with a bare driver line (case 2).
cat > "${WORK}/gcc-colless" <<'EOF'
#!/bin/sh
for a in "$@"; do f="$a"; done
echo "${f}:17: error: ISO C forbids an empty translation unit [-Werror=pedantic]"
echo "cc1: all warnings being treated as errors"
exit 1
EOF
cat > "${WORK}/gcc-bare" <<'EOF'
#!/bin/sh
echo "cc1: all warnings being treated as errors"
exit 1
EOF
chmod +x "${WORK}/gcc-colless" "${WORK}/gcc-bare"

cd "${REPO_ROOT}" || exit 1
OUT="$(CC="${WORK}/gcc-colless" python3 scripts/check_stdlib_analysis.py --nonhost --only src/version.h 2>&1)"
if printf '%s\n' "${OUT}" | grep -q 'FAIL:  NONHOST: src/version.h:17: ISO C forbids an empty translation unit'; then
    ok "1-colless-error-names-file"
else bad "1-colless-error-names-file" "$(printf '%s\n' "${OUT}" | grep FAIL)"; fi

OUT="$(CC="${WORK}/gcc-bare" python3 scripts/check_stdlib_analysis.py --nonhost --only src/version.h 2>&1)"
if printf '%s\n' "${OUT}" | grep -q 'FAIL:  NONHOST: src/version.h: cc1: all warnings being treated as errors'; then
    ok "2-unlocated-error-names-source"
else bad "2-unlocated-error-names-source" "$(printf '%s\n' "${OUT}" | grep FAIL)"; fi

DRY="$(make -n --no-print-directory lint-nonhost LINT_CC=c1961-cc 2>/dev/null)"
if printf '%s\n' "${DRY}" | grep -q "CC='c1961-cc' python3 scripts/check_stdlib_analysis.py --nonhost"; then
    ok "3-lint-nonhost-uses-LINT_CC"
else bad "3-lint-nonhost-uses-LINT_CC" "$(printf '%s\n' "${DRY}" | grep check_stdlib_analysis)"; fi

R="$(python3 - "${WORK}" <<'PY'
import importlib.util, os, sys
work = sys.argv[1]
spec = importlib.util.spec_from_file_location("csa", "scripts/check_stdlib_analysis.py")
m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
here = m.HOST_NAME
other = "linux" if here == "darwin" else "darwin"
p = os.path.join(work, "skip.txt")
open(p, "w").write(
    "nonhost-uncoverable@%s src/a.c  # other host\n"
    "nonhost-uncoverable@%s src/b.c  # this host\n" % (other, here))
m.SKIPLIST = p
e, err = m.load_skiplist()
print("4a" if ("nonhost-uncoverable", "src/a.c") not in e and not err else "x4a")
print("4b" if ("nonhost-uncoverable", "src/b.c") in e else "x4b")
open(p, "w").write("nonhost@darwin src/c.c  # bad\nnonhost-uncoverable@bsd src/d.c  # bad\n")
e, err = m.load_skiplist()
print("4c" if len(err) == 2 and not e else "x4c %r" % err)
PY
)"
for k in 4a 4b 4c; do
    if printf '%s\n' "${R}" | grep -qx "${k}"; then ok "${k}-host-scoped-skip"
    else bad "${k}-host-scoped-skip" "${R}"; fi
done

echo "C1961: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
