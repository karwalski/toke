#!/usr/bin/env bash
# C256_os_getcwd_caller_owned.sh — os.getcwd returns what os.tki declares:
# a caller-owned string (story 135.23).
#
# THE DEFECT.  stdlib/os.tki declares os.getcwd "ownership": "caller", and
# tk_os_getcwd returned `static char buf[4096]`: a caller that freed the
# result freed a static buffer, two results were the same pointer (the second
# call rewrote the first), and a working directory of 4096 bytes or more came
# back as "".  os.read (also "caller") returned the literal "" on failure.
#
# WHAT IS ASSERTED, with os.c built under ASan+UBSan and driven from C:
#   1. two calls return two different pointers, both equal to getcwd(3)
#   2. both can be free()d (ASan aborts on a free of a non-heap address)
#   3. inside a directory whose path exceeds 4096 bytes the full path comes
#      back, not ""
#   4. a failed os.read (bad fd) returns an empty string that can be freed
# and, end to end, a compiled toke program prints os.getcwd() == $PWD.
#
# Negative control: C256_STDLIB=<dir> builds against another stdlib tree; a
# copy with main's os.c fails cases 1-4 (ASan reports the bad free).
#
# Story: 135.23
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
CC="${CC:-cc}"
S="${C256_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c256_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C256: os.getcwd returns a caller-owned string (135.23)"
echo "------------------------------------------------------"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include "os.h"
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
int main(void) {
    char want[8192];
    if (!getcwd(want, sizeof want)) { printf("  FAIL: getcwd(3)\n"); return 1; }
    char *a = (char *)(intptr_t)tk_os_getcwd();
    char *b = (char *)(intptr_t)tk_os_getcwd();
    CHECK(a && b && a != b && strcmp(a, want) == 0 && strcmp(b, want) == 0,
          "two calls: two different pointers, both the working directory");
    free(a); free(b);
    CHECK(1, "both results were free()d (ASan would have aborted before this line)");

    /* A path longer than 4096 bytes: 20 levels of 250-byte names. */
    char name[251]; memset(name, 'd', 250); name[250] = '\0';
    for (int i = 0; i < 20; i++) {
        if (mkdir(name, 0700) != 0 || chdir(name) != 0) { printf("  FAIL: mkdir/chdir level %d\n", i); return 1; }
    }
    char *deep = (char *)(intptr_t)tk_os_getcwd();
    size_t dl = deep ? strlen(deep) : 0;
    CHECK(dl > 5000 && strstr(deep, want) == deep,
          "a working directory longer than 4096 bytes comes back whole");
    if (!(dl > 5000)) printf("        got %zu bytes\n", dl);
    free(deep);

    char *r = (char *)(intptr_t)tk_os_read(-1, 16);
    CHECK(r && r[0] == '\0', "os.read on a bad fd returns an empty string");
    free(r);
    CHECK(1, "and that empty string was free()d");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# 127.268: the stdlib link set comes from tkc's own dependency table
# (src/stdlib_deps.c via --emit-deps), not a hand list.
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" os)" \
    || { echo "  FAIL: stdlib link set (see above)"; echo "Results: 0 passed, 1 failed"; exit 1; }
LINK=(); while IFS= read -r l; do LINK+=("$l"); done <<< "${LINKSET}"
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o t t.c "${LINK[@]}" -lm -lpthread > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; echo "C256: 0 passed, 1 failed"; exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t > t.out 2>&1
rc=$?
grep -a '^  PASS\|^  FAIL' t.out
grep -a -c '^  PASS' t.out > n.pass; PASS=$((PASS + $(cat n.pass)))
FAIL=$((FAIL + $(grep -a -c '^  FAIL' t.out)))
if [ "${rc}" -ne 0 ] && ! grep -a -q '^C-driver: [1-9]' t.out; then
    echo "  FAIL: the driver died (rc=${rc}) before finishing:"
    grep -a 'ERROR: AddressSanitizer\|attempting free\|runtime error' t.out | head -3 | sed 's/^/        /'
    FAIL=$((FAIL + 1))
fi

# End to end: a toke program's os.getcwd() is its working directory.
mkdir e2e && cd e2e
cat > p.tk <<'TOKE'
m=main;i=os:std.os;i=io:std.io;f=main():i64{io.println(os.getcwd());<0};
TOKE
if TKC_STDLIB_DIR="${S}" "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    got="$(./p 2>&1)"
    if [ "${got}" = "$(pwd -P)" ] || [ "${got}" = "$(pwd)" ]; then
        echo "  PASS: a toke program prints os.getcwd() == its working directory"; PASS=$((PASS + 1))
    else
        echo "  FAIL: toke program printed '${got}', expected '$(pwd -P)'"; FAIL=$((FAIL + 1))
    fi
else
    echo "  FAIL: p.tk did not compile"; sed 's/^/        /' p.log | head -4; FAIL=$((FAIL + 1))
fi

echo "------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
