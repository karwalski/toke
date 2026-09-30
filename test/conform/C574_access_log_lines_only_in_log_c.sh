#!/usr/bin/env bash
# C574_access_log_lines_only_in_log_c.sh — only log.c formats access-log
# lines; router_use_log is gone (121.41 (2)).
#
# THE DEFECT.  router.c's router_use_log middleware (Story 27.1.11) wrote the
# request path raw into a Common Log Format line (`"%s %s HTTP/1.1" %d`) or a
# JSON object (`"path":"%s"`): a path holding `"` or a newline forged a field
# or a whole line.  metrics.c had the same pair (deleted by H2, 3c58a049).
# No .tki and no C caller outside the unit tests reached router_use_log, so
# it is deleted rather than escaped; log.c's tk_access_log_* escape every
# field and remain the one writer.
#
# Checks, over src/stdlib/*.c of ${C574_ROOT:-repo} (comments excluded):
#   1 no file but log.c holds a format string with a raw `%s` request field
#     in either line shape: `\"%s %s HTTP/` (CLF) or `\"path\":\"%s\"` /
#     `\"method\":\"%s\"` (JSON)
#   2 router_use_log is neither defined nor declared (router.c, router.h)
#   3 the control: log.c's own CLF writer is found by the same scan (so the
#     scan cannot pass by matching nothing)
#
# Negative control: C574_ROOT=<main at cde89021> — 1 red (router.c: the
# CLF and JSON formats), 2 red (2 hits); 3 green on both.
#
# Story: 121.41 (2)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROOT="${C574_ROOT:-${REPO_ROOT}}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
scan="$(python3 - "${ROOT}" <<'PY'
import glob, os, re, sys
root = sys.argv[1]
pats = [re.compile(r'\\"%s %s HTTP/'), re.compile(r'\\"(?:path|method)\\":\\"%s\\"'),
        re.compile(r'HTTP/1\.1\\" %d')]
out, logc, uselog = [], 0, 0
for p in sorted(glob.glob(os.path.join(root, 'src', 'stdlib', '*.[ch]'))):
    s = open(p, errors='replace').read()
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S); s = re.sub(r'//[^\n]*', '', s)
    b = os.path.basename(p)
    uselog += len(re.findall(r'\brouter_use_log\s*\(', s))
    for rx in pats:
        n = len(rx.findall(s))
        if not n: continue
        if b == 'log.c': logc += n
        else: out.append(f'{b}:{n}')
print(' '.join(sorted(set(out))) or 'none'); print(uselog); print(logc)
PY
)"
expect "1-no-raw-access-line-outside-log.c" "$(printf '%s\n' "${scan}" | sed -n 1p)" "none"
expect "2-router_use_log-gone"              "$(printf '%s\n' "${scan}" | sed -n 2p)" "0"
lc="$(printf '%s\n' "${scan}" | sed -n 3p)"
expect "3-control-log.c-writer-seen"        "$([ "${lc:-0}" -ge 1 ] && echo seen || echo "missed (${lc})")" "seen"
echo "C574: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
