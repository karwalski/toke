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
#   * 135.3j (7): the diagnostics array is populated -- a type error carries
#     its code (4031), line and message; tkc_get_diagnostics() agrees; a
#     diagnostic with no fix has no "fix" key; nothing reaches stderr; and
#     tkc_version() states VERSION, not the old literal "tkc 0.3.0-wasm".
#     Until 135.3j every result carried "diagnostics":[] (measured on main
#     c8575da: the E4031 case below returned error_count 1 and an empty
#     array, and wrote the JSON record to stderr instead).
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
const char *tkc_get_diagnostics(const char *source);
void tkc_free(void *ptr);
int main(int argc, char **argv)
{
    if (argc < 2) return 2;
    if (strcmp(argv[1], "version") == 0) { puts(tkc_version()); return 0; }
    if (argc < 3) return 2;
    const char *r = strcmp(argv[1], "check") == 0 ? tkc_check_source(argv[2])
                  : strcmp(argv[1], "format") == 0 ? tkc_format_source(argv[2])
                  : strcmp(argv[1], "diag") == 0 ? tkc_get_diagnostics(argv[2])
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

# 135.3j (7): the diagnostics themselves.
want_v="tkc $(tr -d '[:space:]' < VERSION)-wasm"
expect "tkc_version() states VERSION" "${v}" "${want_v}"
bad='m=main;f=main():i64{let x:bool=1;<0};'
r="$(json check "${bad}")"
expect "type error: first diagnostic code" "$(field code "${r}")" "4031"
expect "type error: first diagnostic line" "$(field line "${r}")" "1"
case "$(field message "${r}")" in
    MISSING|"") fail "type error: diagnostic has no message" ;;
    *) pass "type error: diagnostic carries its message" ;;
esac
# The fix is the CLI's fix, byte for byte, and absent where the CLI's is.
TKC="${TKC:-${REPO_ROOT}/toke}"
printf '%s' "${bad}" > "${WORK}/bad.tk"
expect "type error: fix = the CLI's fix" "$(field fix "${r}")" \
    "$("${TKC}" --check "${WORK}/bad.tk" 2>&1 | awk '{print; exit}' | awk -v f='"fix":' '{i=index($0,f); if(!i){print "MISSING"; exit} s=substr($0,i+length(f)+1); j=index(s,"\""); print substr(s,1,j-1)}')"
nofix='m=main;f=main():i64{<"a"};'
printf '%s' "${nofix}" > "${WORK}/nofix.tk"
cli_nofix="$("${TKC}" --check "${WORK}/nofix.tk" 2>&1 | awk '/"fix":/{c++} END{print c+0}')"
rn="$(json check "${nofix}")"
case "${cli_nofix}:${rn}" in
    0:*'"fix":'*) fail "no-fix E4031: a \"fix\" key the CLI does not emit" ;;
    0:*)          pass "no-fix E4031: no fix key, as on the CLI" ;;
    *)            fail "no-fix E4031: control case now has a CLI fix -- pick another" ;;
esac
r2="$(json diag "${bad}")"
expect "tkc_get_diagnostics agrees with tkc_check_source" "${r2}" "${r}"
err="$("${B}" check "${bad}" 2>&1 >/dev/null)"
expect "nothing is written to stderr" "${err}" ""
r="$(json check 'm=main;f=main():i64{<0')"
case "$(field code "${r}")" in
    MISSING) fail "parse error: diagnostics array is empty" ;;
    *) pass "parse error: diagnostics array is populated" ;;
esac

echo "Results: ${PASS} passed, ${FAIL} failed"
# Positive assertion: the build check and all API checks ran.
[ $((PASS + FAIL)) -ge 19 ] || { echo "C135: only $((PASS + FAIL)) checks ran"; exit 1; }
[ "${FAIL}" -eq 0 ]
