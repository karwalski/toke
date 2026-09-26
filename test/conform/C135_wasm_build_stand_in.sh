#!/usr/bin/env bash
# C135_wasm_build_stand_in.sh — the WASM build's C compiles, links and works,
# checked with the host compiler because there is no emcc here or in CI.
#
# Story: 136.64.
#
# THE DEFECT.  Makefile.wasm (the browser playground build) had not been
# buildable for some time and nothing noticed, because nothing built it:
#   * src/wasm_api.c called parse() with 4 arguments; it takes 5 (Profile);
#   * the link lacked 8 symbols -- the type checker resolves stdlib calls
#     through tables in llvm.c and stdlib_deps.c, which the WASM source list
#     excluded "because there is no codegen in the browser".
#
# WHAT THIS SUITE DOES.  Reads Makefile.wasm's own source list and flags
# (`make -f Makefile.wasm print-srcs`, so it cannot drift from that file),
# compiles and links them with the HOST compiler under the same
# -Wall -Wextra -Wpedantic -Werror -DTKC_WASM_BUILD, plus a small driver in
# place of the JavaScript caller, and asserts on what the exported API returns:
#   * tkc_version() is non-empty;
#   * tkc_check_source(): a valid program -> "success":true at stage "types";
#     a type error -> "success":false, stage "types"; a parse error ->
#     "success":false, stage "parse";
#   * tkc_format_source() returns the canonical formatting.
#
# WHAT IT DOES NOT PROVE.  That emcc accepts the code, or anything about the
# .wasm/.js artefacts.  It is a stand-in for an emcc job, not a replacement.
#
# Negative control: C135_EXTRA_SRCS_DROP='src/llvm.c' removes one source from
# the list and the link must fail (the pre-136.64 state).

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
MAKE_BIN="${MAKE:-make}"
CC="${CC:-cc}"
cd "${REPO_ROOT}" || exit 1

PASS=0
FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS+1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL+1)); }

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c135_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C135: WASM build, host-compiled stand-in (136.64)"
echo "--------------------------------------------------"

"${MAKE_BIN}" --no-print-directory -f Makefile.wasm print-srcs > "${WORK}/vars" 2>&1 || {
    fail "make -f Makefile.wasm print-srcs failed: $(sed -n '1p' "${WORK}/vars")"
    echo "C135: ${PASS} passed, ${FAIL} failed"; exit 1; }
SRCS="$(sed -n 's/^SRCS=//p' "${WORK}/vars")"
# CFLAGS minus TKC_STDLIB_DIR, whose inner quotes do not survive an echo; it
# is re-added below with its Makefile.wasm value.
WFLAGS="$(sed -n 's/^CFLAGS=//p' "${WORK}/vars" | tr ' ' '\n' | awk 'NF && index($0,"-DTKC_STDLIB_DIR=")!=1' | tr '\n' ' ')"
for d in ${C135_EXTRA_SRCS_DROP:-}; do
    SRCS="$(printf '%s\n' ${SRCS} | awk -v d="${d}" '$0!=d' | tr '\n' ' ')"
done
nsrc="$(printf '%s\n' ${SRCS} | awk 'NF{c++} END{print c+0}')"
case " ${WFLAGS} " in
    *" -DTKC_WASM_BUILD "*) ;;
    *) fail "Makefile.wasm CFLAGS no longer carry -DTKC_WASM_BUILD: ${WFLAGS}" ;;
esac
case " ${SRCS} " in
    *" src/wasm_api.c "*) ;;
    *) fail "Makefile.wasm SRCS no longer include src/wasm_api.c" ;;
esac

cat > "${WORK}/drv.c" <<'CEOF'
#include <stdio.h>
#include <string.h>
const char *tkc_check_source(const char *source);
const char *tkc_format_source(const char *source);
const char *tkc_version(void);
void tkc_free(void *ptr);
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (strcmp(argv[1], "version") == 0) { puts(tkc_version()); return 0; }
    if (argc < 3) return 2;
    const char *r = strcmp(argv[1], "check") == 0 ? tkc_check_source(argv[2])
                  : strcmp(argv[1], "format") == 0 ? tkc_format_source(argv[2])
                  : NULL;
    if (!r) return 3;
    puts(r);
    tkc_free((void *)r);
    return 0;
}
CEOF

# shellcheck disable=SC2086
if "${CC}" ${WFLAGS} '-DTKC_STDLIB_DIR="/stdlib"' -Isrc -o "${WORK}/wasm_standin" \
        ${SRCS} "${WORK}/drv.c" -lm -lz -lpthread > "${WORK}/cc.log" 2>&1; then
    pass "Makefile.wasm's ${nsrc} sources compile under its flags and link"
else
    fail "Makefile.wasm's ${nsrc} sources do not build: $(awk '/error|Undefined|undefined/{print; c++} c==3{exit}' "${WORK}/cc.log" | tr '\n' '|')"
    echo "C135: ${PASS} passed, ${FAIL} failed"; exit 1
fi

B="${WORK}/wasm_standin"
json() { "${B}" "$@" 2>/dev/null; }
field() {  # FIELD JSON -> value of "FIELD": (string or bare), via awk
    printf '%s' "$2" | awk -v f="\"$1\":" '{i=index($0,f); if(!i){print "MISSING"; exit}
        s=substr($0,i+length(f)); if(substr(s,1,1)=="\""){s=substr(s,2); j=index(s,"\""); print substr(s,1,j-1)}
        else {match(s,/^[^,}]*/); print substr(s,1,RLENGTH)}}'
}
expect() {  # WHAT GOT WANT
    if [ "$2" = "$3" ]; then pass "$1 = $2"; else fail "$1: got '$2', want '$3'"; fi
}

v="$("${B}" version)"
if [ -n "${v}" ]; then pass "tkc_version() = ${v}"; else fail "tkc_version() is empty"; fi

r="$(json check 'm=main;f=main():i64{<0};')"
expect "valid program: success" "$(field success "${r}")" "true"
expect "valid program: stage"   "$(field stage "${r}")"   "types"

r="$(json check 'm=main;f=main():i64{let x:bool=1;<0};')"
expect "type error: success" "$(field success "${r}")" "false"
expect "type error: stage"   "$(field stage "${r}")"   "types"

r="$(json check 'm=main;f=main():i64{<0')"
expect "parse error: success" "$(field success "${r}")" "false"
expect "parse error: stage"   "$(field stage "${r}")"   "parse"

r="$(json format 'm=main;f=main():i64{let x=1;<x};')"
expect "format: success"   "$(field success "${r}")"   "true"
expect "format: formatted" "$(field formatted "${r}")" 'm=main;\nf=main():i64 {\n  let x=1;\n  <x\n}\n'

echo "C135: ${PASS} passed, ${FAIL} failed"
# Positive assertion: the build check and all nine API checks ran.
[ $((PASS + FAIL)) -ge 10 ] || { echo "C135: only $((PASS + FAIL)) checks ran"; exit 1; }
[ "${FAIL}" -eq 0 ]
