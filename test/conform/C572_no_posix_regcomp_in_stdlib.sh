#!/usr/bin/env bash
# C572_no_posix_regcomp_in_stdlib.sh — no stdlib C file compiles a pattern
# with POSIX regcomp; tk_regex.h is the one engine (121.45 (2)).
#
# THE DEFECT.  tk_web_glue.c kept tk_regex_{match,replace,findall}_w (Story
# 78.4.3): POSIX regcomp wrappers that read an invalid pattern as "no match"
# and differ by platform (the reason 137.9 moved str.* onto tk_regex.h).  No
# .tki declares std.regex, so no toke program could reach them, but they were
# linked into every std.http program and listed in the generated declaration
# tables.  They are deleted.
#
# Checks, over ${C572_ROOT:-repo}:
#   1 no `regcomp(` / `regexec(` call in src/stdlib/*.c (comments excluded)
#   2 the generated tables (stdlib_decls_gen.h, stdlib_paramkind_gen.h) name
#     no tk_regex_*_w wrapper
#   3 `i=re:std.regex;` is still E2030 — the wrappers were unreachable, and a
#     std.regex module must not appear without a surface story
#
# Negative control: C572_ROOT=<main at cde89021> — checks 1 and 2 red
# (tk_web_glue.c: 3 regcomp + 3 regexec calls; 3 decls + 3 paramkinds); 3 green.
#
# Story: 121.45 (2)
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
ROOT="${C572_ROOT:-${REPO_ROOT}}"
TKC="${TKC:-${REPO_ROOT}/toke}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
calls="$(python3 - "${ROOT}" <<'PY'
import glob, os, re, sys
hits = []
for p in sorted(glob.glob(os.path.join(sys.argv[1], 'src', 'stdlib', '*.c'))):
    s = open(p, errors='replace').read()
    s = re.sub(r'/\*.*?\*/', '', s, flags=re.S)
    s = re.sub(r'//[^\n]*', '', s)
    for m in re.finditer(r'\b(regcomp|regexec)\s*\(', s):
        hits.append(f'{os.path.basename(p)}:{m.group(1)}')
print(' '.join(hits) if hits else 'none')
PY
)"
expect "1-no-posix-regex-calls" "${calls}" "none"
gen="$(cat "${ROOT}/src/stdlib_decls_gen.h" "${ROOT}/src/stdlib_paramkind_gen.h" 2>/dev/null | grep -c 'tk_regex_[a-z]*_w')"
expect "2-no-regex-wrappers-in-tables" "${gen}" "0"
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
printf 'm=main;i=re:std.regex;\nf=main():i64{\n  let x=re.match("a+";"aaa");\n  <0\n};\n' > "${WORK}/p.tk"
if "${TKC}" --allow-all "${WORK}/p.tk" -o "${WORK}/b" > "${WORK}/log" 2>&1; then got=compiled
else got="$(grep -o '"error_code":"E[0-9]*"' "${WORK}/log" | head -1)"; fi
expect "3-std-regex-not-a-module" "${got}" '"error_code":"E2030"'
echo "C572: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
