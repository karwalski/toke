#!/usr/bin/env bash
# C152_diag_text_opt_in.sh — human-readable diagnostics (source line + caret)
# are an opt-in mode; JSON is the default whatever stdout is (story 123.12b).
#
# THE DEFECT.  main.c chose TEXT whenever stdout was a TTY.  A tool or agent
# that drives tkc through a pseudo-terminal (an editor terminal, an agent
# harness with a pty, `script`) therefore got prose on stderr instead of the
# JSON records the repair loop parses -- and nothing tested --diag-text at all.
#
# WHAT THIS CHECKS
#   1. under a pseudo-terminal, with no flag, every stderr line is a JSON
#      record (needs `script`; SKIP line printed if absent)
#   2. with no flag and no TTY, output is byte-identical to --diag-json
#   3. --diag-text prints `error[E4031]: ...`, ` --> FILE:3:3`, the numbered
#      source line, and a caret at column 3
#   4. --diag-text underlines a multi-character token span: `^~~` under `foo`
#   5. --diag-text and --diag-json are mutually exclusive (usage error)
#
# Negative control: restore `isatty(STDOUT_FILENO) ? DIAG_FMT_TEXT :` in the
# two diag_set_format calls in src/main.c -> case 1 fails (first line is
# `error[E4031]: ...`).
#
# Story: 123.12

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0; SKIP=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
TKC="$(cd "$(dirname "${TKC}")" && pwd)/$(basename "${TKC}")"
WORK="$(mktemp -d /tmp/tkc_c152_XXXXXX)"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

echo "C152: --diag-text is opt-in; JSON by default (123.12b)"

printf 'm=main;\nf=main():i64{\n  let s:str=5;\n  <0\n};\n' > t1.tk
printf 'm=main;\nf=main():i64{\n  <0\n} foo bar;\n' > t3.tk

# 1. pseudo-terminal
if command -v script >/dev/null 2>&1; then
    if script -q /dev/null true >/dev/null 2>&1 < /dev/null; then
        script -q /dev/null "${TKC}" --check t1.tk > pty.out 2>&1 < /dev/null
    else
        script -qc "'${TKC}' --check t1.tk" /dev/null > pty.out 2>&1 < /dev/null
    fi
    # script(1) may echo a ^D and backspaces before the child's output; keep
    # what follows them.  Every remaining non-empty line must be a record.
    if python3 - pty.out 2>/dev/null <<'PY'
import json, re, sys
raw = open(sys.argv[1], encoding="utf-8", errors="replace").read()
lines = [re.sub(r"^(\^D|[\x04\x08])+", "", l).strip()
         for l in raw.replace("\r", "").split("\n")]
lines = [l for l in lines if l]
assert lines, "no output"
for l in lines:
    json.loads(l)
PY
    then
        ok "under a pseudo-terminal the default is JSON"
    else
        bad "under a pseudo-terminal the default is not JSON: $(head -1 pty.out | cut -c1-80)"
    fi
else
    echo "  SKIP: no \`script\` on this host -- the pseudo-terminal case did not run"
    SKIP=$((SKIP+1))
fi

# 2. default == --diag-json, byte for byte
"${TKC}" --check t1.tk > d.out 2>&1
"${TKC}" --check --diag-json t1.tk > j.out 2>&1
if cmp -s d.out j.out && grep -q '"error_code":"E4031"' d.out; then
    ok "no flag is byte-identical to --diag-json"
else
    bad "default differs from --diag-json"
fi

# 3. text: header, locator, numbered line, caret column
"${TKC}" --check --diag-text t1.tk > t.out 2>&1
if grep -q "^error\[E4031\]: type mismatch" t.out \
   && grep -q '^ --> t1.tk:3:3$' t.out \
   && grep -q '^3 |   let s:str=5;$' t.out \
   && grep -q '^  |   ^' t.out; then
    ok "--diag-text shows the source line with a caret"
else
    bad "--diag-text layout:"; sed 's/^/      /' t.out
fi

# 4. span underline
"${TKC}" --check --diag-text t3.tk > u.out 2>&1
if grep -q '^  |   ^~~' u.out; then
    ok "--diag-text underlines a multi-character span"
else
    bad "no span underline:"; sed 's/^/      /' u.out | head -8
fi

# 5. mutually exclusive
"${TKC}" --check --diag-text --diag-json t1.tk > x.out 2>&1; rc=$?
if [ "${rc}" -ne 0 ] && [ "${rc}" -ne 1 ] && grep -q 'mutually exclusive' x.out; then
    ok "--diag-text with --diag-json is a usage error (rc ${rc})"
else
    bad "--diag-text --diag-json: rc ${rc}: $(head -1 x.out)"
fi

echo "C152: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
[ "${FAIL}" -eq 0 ]
