#!/usr/bin/env bash
# C2286_yaml_truncated_literal.sh -- yaml.fromjson never reads past the end
# of a document that stops inside true/false/null (story 127.544).
#
# THE DEFECT (found auditing yaml.c for 127.542; measured on eca5062f under
# ASan+UBSan): json_skip_value skipped the literals as a fixed p+4 (true,
# null) or p+5 (false), whatever followed.  A document ending inside one --
# `[t`, `{"a":n`, `[1,f` -- left the cursor past the string's terminator, and
# every later read was out of bounds: heap-buffer-overflow under ASan, and
# without it the converter walked whatever memory followed (`[t` passed as a
# C string from the command line never returned).
#
# THE FIX: the literals are skipped over at most 4 / 5 bytes and never past
# the terminator.  A complete literal converts exactly as before.
#
# WHAT IS ASSERTED: yaml_from_json, built with ASan+UBSan and given an
# exactly-sized heap copy of each input (so a read one byte past the end
# aborts), returns the expected text for each truncated literal, and the
# controls (complete literals in a sequence and in a mapping) convert
# unchanged.  The fuzz input
# test/fuzz/corpus-adversarial/yaml_truncated_literal_127544.json is replayed
# by make fuzz-smoke.
#
# Negative control: C2286_STDLIB=<dir> builds against another tree's
# src/stdlib.  The base eca5062f and the v3.2.0 tree abort with ASan's
# heap-buffer-overflow on the first truncated literal.
#
# Story: 127.544
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
S="${C2286_STDLIB:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c2286_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
echo "C2286: yaml.fromjson on a document truncated inside a literal (127.544)"
echo "-----------------------------------------------------------------------"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "yaml.h"
static int fails = 0;
/* An exactly-sized heap copy, so ASan catches a read past the terminator. */
static void check(const char *in, const char *want) {
    size_t n = strlen(in);
    char *h = malloc(n + 1);
    memcpy(h, in, n + 1);
    const char *got = yaml_from_json(h);
    if (strcmp(got, want) == 0) printf("  PASS: %s\n", in);
    else { printf("  FAIL: %s -> [%s], want [%s]\n", in, got, want); fails++; }
    free(h);
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    check("[t", "- t");
    check("{\"a\":n", "a: n");
    check("[1,f", "- 1\n- f");
    check("t", "t");
    /* controls */
    check("[true,false,null]", "- true\n- false\n- null");
    check("{\"a\":null,\"b\":true}", "a: null\nb: true");
    return fails ? 1 : 0;
}
C
if ! "${CC}" -fsanitize=address,undefined -fno-sanitize-recover=all \
        -fno-omit-frame-pointer -g -O1 -std=c99 -D_GNU_SOURCE \
        -iquote "${S}" -o t t.c "${S}/yaml.c" "${S}/str.c" -DTK_STR_NO_HEAP \
        > cc.log 2>&1; then
    echo "  FAIL: build"; head -8 cc.log | sed 's/^/        /'; exit 1
fi
ASAN_OPTIONS=detect_leaks=0 perl -e 'alarm 20; exec @ARGV' ./t > out.log 2>&1
rc=$?
grep -E '^  (PASS|FAIL)' out.log
grep -m1 'ERROR: AddressSanitizer' out.log | sed 's/^/        /'
echo "-----------------------------------------------------------------------"
if [ "${rc}" -eq 0 ]; then echo "C2286: passed"; exit 0; fi
echo "C2286: FAILED (exit ${rc})"; exit 1
