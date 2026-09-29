#!/usr/bin/env bash
# C436_consumer_surface_withdrawn.sh — the standard library carries no surface
# that exists only because one downstream project calls it (story 136.45).
#
# THE DEFECT.  Toke's stdlib held wrappers whose only specification was one
# consumer's source tree (loke): std.string and std.fs -- bare second names
# for std.str and std.file -- and a block of str aliases (str.fromi32,
# str.fromu32, str.strip, str.bytelen, ...) commented "loke uses these".  It
# also hard-coded that project's identity: a `loke://` URL scheme in
# webview.c (trusted by default, served by a reserved "loke_scheme" handler)
# and `~/.loke/models/` documented as the stdlib's model-directory convention.
#
# THE DECISION (owner, 2026-09-22): withdraw, do not promote.  The consumer
# renames on its side (std.string -> std.str, std.fs -> std.file,
# str.fromi32 -> str.fromint, ...).
#
# WHAT IS ASSERTED
#   1. `i=x:std.string;` and `i=x:std.fs;` are rejected at the import (E2030)
#   2. each withdrawn str alias is not a member of std.str (E4027)
#   3. no C string literal in src/stdlib/*.c|*.h contains "loke" -- a
#      consumer's identity is not a runtime value of the standard library
#      (comments that cite a loke report ID are history, not surface)
#   4. no `.loke/` path is published in src/stdlib or docs/stdlib
#   5. the canonical spellings the consumer renames to still compile
#
# Negative control: run against main (8efba9ea): 1-4 all fail (the imports
# resolve, the aliases are members, webview.c holds "loke" and "loke_scheme",
# infer.h/infer.md publish ~/.loke/models/).
#
# Story: 136.45
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
SRC="${C436_ROOT:-${REPO_ROOT}}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c436_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}"
ok()  { echo "  PASS: $1"; PASS=$((PASS+1)); }
bad() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }
echo "C436: no consumer-specific stdlib surface (136.45)"
echo "---------------------------------------------------"

# 1. the withdrawn modules fail at the import
for mod in string fs; do
    printf 'm=t;\ni=x:std.%s;\nf=main():i64{<0};\n' "$mod" > imp_$mod.tk
    "${TKC}" --check imp_$mod.tk > imp_$mod.log 2>&1
    if /usr/bin/grep -a -q '"error_code":"E2030"' imp_$mod.log; then ok "import std.$mod is E2030"
    else bad "import std.$mod was not rejected with E2030: $(head -c 300 imp_$mod.log)"; fi
done

# 2. the withdrawn str aliases are not members
for m in fromu16 fromu32 fromu64 fromint64 fromi32 fromf32 bytelen pad strip trimws \
         wordcount splitfirst splitwhitespace; do
    case "$m" in
        pad)                      call='str.pad("a";3;32)' ;;
        splitfirst)               call='str.splitfirst("a b";" ")' ;;
        bytelen|strip|trimws|wordcount|splitwhitespace) call="str.$m(\"a\")" ;;
        *)                        call="str.$m(1)" ;;
    esac
    printf 'm=t;\ni=str:std.str;\nf=main():i64{\n  let v=%s;\n  <0\n};\n' "$call" > a_$m.tk
    "${TKC}" --check a_$m.tk > a_$m.log 2>&1
    if /usr/bin/grep -a -q '"error_code":"E4027"' a_$m.log; then ok "str.$m is not a member (E4027)"
    else bad "str.$m still resolves: $(head -c 300 a_$m.log)"; fi
done

# 3. no "loke" inside a C string literal in the stdlib sources
cat > lits.py <<'PY'
import re, sys
from pathlib import Path
tok = re.compile(r'/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'', re.S)
hits = []
for p in sorted(Path(sys.argv[1]).glob("*.[ch]")):
    t = p.read_text(errors="replace")
    for m in tok.finditer(t):
        s = m.group(0)
        if s.startswith('"') and "loke" in s.lower():
            hits.append(f"{p.name}:{t.count(chr(10), 0, m.start()) + 1}:{s[:40]}")
print(" ".join(hits))
PY
lits="$(python3 lits.py "${SRC}/src/stdlib")"
if [ -z "${lits}" ]; then ok "no C string literal in src/stdlib contains \"loke\""
else bad "consumer identity in stdlib string literals: ${lits}"; fi

# 4. no .loke/ path published
paths="$(/usr/bin/grep -a -rln '\.loke/' "${SRC}/src/stdlib" "${SRC}/docs/stdlib" 2>/dev/null | tr '\n' ' ')"
if [ -z "${paths}" ]; then ok "no .loke/ path in src/stdlib or docs/stdlib"
else bad ".loke/ path published in: ${paths}"; fi

# 5. the canonical spellings compile and run
cat > canon.tk <<'TOKE'
m=canon;
i=io:std.io;
i=str:std.str;
i=file:std.file;
f=main():i64{
  let a=str.fromint(7);
  let b=str.trim("  x  ");
  let c=str.len("abc");
  let d=str.padright("ab";4;42);
  io.println("\(a) \(b) \(c) \(d)");
  <0
};
TOKE
if "${TKC}" canon.tk -o canon > canon.log 2>&1 && [ "$(./canon)" = "7 x 3 ab**" ]; then
    ok "canonical renames compile and run (str.fromint, str.trim, str.len, str.padright, std.file)"
else bad "canonical spellings: $(head -c 300 canon.log) / $(./canon 2>&1)"; fi

echo "---------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
