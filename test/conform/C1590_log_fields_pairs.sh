#!/usr/bin/env bash
# C1590_log_fields_pairs.sh -- log.info/warn/error/debug read `fields` as the
# declared [[str]] pairs (story 127.400).
#
# std.log's .tki declares `fields` as [[str]] and docs/stdlib/log.md passes
# @(@("host"; "0.0.0.0"); @("port"; "8080")).  log_glue.c used to read the
# OUTER array as a flat interleaved key/value list and cast each inner array
# handle to a char *, so every field printed the bytes of an array block as
# text.  It now reads each element through tk_array.h: slot 0 = key,
# slot 1 = value; an element with fewer than two slots is skipped and slots
# past the second are ignored.
#
# WHAT IS ASSERTED (toke, run under TK_STR_VERIFY=1), each line exact with the
# "ts" field removed:
#   1. JSON: the documented example writes "host":"0.0.0.0","port":"8080" in
#      order after msg; built (str.concat) keys/values are read whole; a quote
#      in a value is escaped; a one-slot pair is skipped; a three-slot pair
#      keeps its first two; @() writes no fields; warn/error/debug carry their
#      fields too (debug under TK_LOG_LEVEL=DEBUG).
#   2. text (log.setformat("text")): the same pairs as logfmt key=value.
#
# Negative control: C1590_STDLIB=<dir> uses another tree's src/stdlib (and
# TKC=<tkc> another compiler).  Run with toke main's tkc and src/stdlib
# (05e02965, the same decode_log_fields as the base 76574959): 2 passed,
# 8 failed -- every fields case prints garbage keys; only the @() cases pass.
# Measured: this branch 10 passed.
#
# Story: 127.400
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1590_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1590_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C1590: log fields are read as [[str]] pairs (127.400)"
echo "--------------------------------------------------------------------------------"

cat > p.tk <<'TOKE'
m=p;
i=log:std.log;
i=str:std.str;
f=emitall():i64{
  log.info("server started"; @(@("host"; "0.0.0.0"); @("port"; "8080")));
  let k=str.concat("us";"er");
  let v=str.concat("car";"ol");
  log.info("built"; @(@(k; v)));
  log.warn("odd"; @(@("q"; "a\"b"); @("solo"); @("x"; "1"; "extra")));
  log.error("none"; @());
  log.debug("dbg"; @(@("d"; "4")));
  <0
};
f=main():i64{
  emitall();
  log.setformat("text");
  emitall();
  <0
};
TOKE
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "p.tk did not compile"; sed 's/^/        /' p.log | head -6
else
    TK_STR_VERIFY=1 TK_LOG_LEVEL=DEBUG ./p > p.out 2> p.err; rc=$?
    if [ "${rc}" -ne 0 ]; then fail "p exited ${rc}: '$(head -c 200 p.err)'"; fi
    python3 - p.err > py.out <<'PY'
import re, sys
L = open(sys.argv[1], encoding="utf-8", errors="replace").read().split("\n")
J = [re.sub(r',"ts":"[^"]*"', "", l) for l in L if l.startswith("{")]
T = [l for l in L if l.startswith("[")]
want_j = [
    ('documented example: host and port in order',
     '{"level":"info","msg":"server started","host":"0.0.0.0","port":"8080"}'),
    ('built key and value read whole',
     '{"level":"info","msg":"built","user":"carol"}'),
    ('warn: quote escaped, one-slot pair skipped, three-slot pair keeps two',
     '{"level":"warn","msg":"odd","q":"a\\"b","x":"1"}'),
    ('error with @() writes no fields',
     '{"level":"error","msg":"none"}'),
    ('debug carries its field',
     '{"level":"debug","msg":"dbg","d":"4"}'),
]
want_t = [
    ('documented example', '[INFO] msg="server started" host=0.0.0.0 port=8080'),
    ('built key and value', '[INFO] msg=built user=carol'),
    ('warn pairs', '[WARN] msg=odd q="a\\"b" x=1'),
    ('error with @()', '[ERROR] msg=none'),
    ('debug field', '[DEBUG] msg=dbg d=4'),
]
for name, (lines, want) in (("json", (J, want_j)), ("text", (T, want_t))):
    if len(lines) != len(want):
        print("FAIL %s: %d lines, want %d: %r" % (name, len(lines), len(want), lines[:6]))
    for i, (desc, w) in enumerate(want):
        got = lines[i] if i < len(lines) else "<missing>"
        if got == w:
            print("PASS %s: %s" % (name, desc))
        else:
            print("FAIL %s: %s: got %r want %r" % (name, desc, got[:160], w))
PY
    while IFS= read -r line; do
        case "${line}" in
            PASS\ *) pass "${line#PASS }" ;;
            FAIL\ *) fail "${line#FAIL }" ;;
        esac
    done < py.out
fi

echo "--------------------------------------------------------------------------------"
echo "C1590: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
