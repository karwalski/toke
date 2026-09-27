#!/usr/bin/env bash
# C230_multi_module_type_errors_after_name_errors.sh — the multi-module driver
# (tkc a.tk b.tk --out BIN, link_multi_module in src/main.c) recovers from
# name-resolution errors exactly as the single-file path does (story 123.13,
# item 1; the rule itself is 123.12c, tested for one file by C151).
#
# THE GAP.  123.12c taught process_file to type-check the untainted functions
# of a file whose only name errors sit inside function bodies. The
# multi-module driver has its own copy of the pipeline and still stopped at
# the first failing phase, so the same program reported E3011 alone when
# built as part of a project and E3011 + E4031 when checked on its own.
#
# The migrate probe (source_needs_migration) is deliberately NOT changed: it
# suppresses diagnostics and answers one bit ("does this compile?"), and any
# name error already makes that answer "no", so continuing into the type
# checker could not change what it returns.
#
# Cases (every one: exit 1, and no binary written):
#   1. name error in one body + type error in another, in the SECOND module
#      -> E3011 E4031 (the E4031 is new)
#   2. the tainted body's own cascade is not reported -> E3011 only
#   3. a name error in a signature -> no continuation -> E3011 only
#   4. the same recovery in the FIRST module; the build stops there, so the
#      second module (which has its own type error) is never reached
#
# Negative control: delete the typecheck_after_name_errors call in
# link_multi_module -> cases 1 and 4 fail (E3011 alone).
#
# Story: 123.13

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "SKIP: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d /tmp/tkc_c230_XXXXXX)"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"

echo "C230: multi-module driver recovers after name errors (123.13)"

build() {  # files... -> RC, GOT
    rm -f app.bin
    "${TKC}" --diag-json "$@" --out app.bin > out.json 2>&1; RC=$?
    GOT="$(sed -n 's/.*"error_code":"\([EW][0-9]*\)".*/\1/p' out.json | tr '\n' ' ' | sed 's/ $//')"
}
expect() {  # name want-codes files...
    local name="$1" want="$2"; shift 2
    build "$@"
    if [ "${GOT}" = "${want}" ] && [ "${RC}" -eq 1 ] && [ ! -e app.bin ]; then
        echo "  PASS: ${name}"; PASS=$((PASS+1))
    else
        echo "  FAIL: ${name}: want '${want}' rc 1 no binary, got '${GOT}' rc ${RC}$([ -e app.bin ] && echo ' +binary')"
        sed 's/^/      /' out.json | cut -c1-160 | head -5; FAIL=$((FAIL+1))
    fi
}

cat > lib.tk <<'X'
m=lib;
f=dbl(x:i64):i64{
  <x*2
};
X
cat > app.tk <<'X'
m=app;
i=lib:lib;
f=a():i64{
  <zzz
};
f=main():i64{
  let s:str=5;
  <lib.dbl(2)
};
X
expect "second module: name error in one body, type error in another" "E3011 E4031" lib.tk app.tk

cat > cascade.tk <<'X'
m=app;
i=lib:lib;
f=a():i64{
  let s:str=zzz;
  let t:bool=7;
  <0
};
f=main():i64{
  <lib.dbl(2)
};
X
expect "tainted body reports nothing beyond its name error" "E3011" lib.tk cascade.tk

cat > sig.tk <<'X'
m=app;
i=lib:lib;
f=a(x:$nosuch):i64{
  <0
};
f=main():i64{
  let s:str=5;
  <lib.dbl(2)
};
X
expect "name error in a signature: no continuation" "E3011" lib.tk sig.tk

cat > badlib.tk <<'X'
m=lib;
f=dbl(x:i64):i64{
  <qqq
};
f=half(x:i64):i64{
  let b:bool=x;
  <x/2
};
X
cat > app2.tk <<'X'
m=app;
i=lib:lib;
f=main():i64{
  let s:str=5;
  <lib.dbl(2)
};
X
expect "first module recovers; the build stops before the second" "E3011 E4031" badlib.tk app2.tk

echo "C230: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
