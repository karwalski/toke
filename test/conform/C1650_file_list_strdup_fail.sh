#!/usr/bin/env bash
# C1650_file_list_strdup_fail.sh -- file_list fails cleanly when strdup fails
# (story 127.414).
#
# file_list() copied each directory entry with an unchecked strdup, so an
# allocation failure stored a NULL entry and the call returned success; through
# the glue (tk_file_list_w / tk_file_listdir_w) toke then received a [str]
# holding a null str, which any .len or interpolation dereferences.  It now
# frees the entries made so far, closes the directory and fails with
# FILE_ERR_IO "allocation failed", as file_glob and file_listall already did.
#
# HOW THE FAILURE IS INJECTED: the harness defines strdup itself, so the
# static link binds file.o's call to it instead of libc's.  It behaves as
# strdup except while armed, when the k-th call whose argument is one of the
# fixture's three names returns NULL (other strdup callers are not counted).
# Bounded: k = 1..3 over a directory of three entries.
#
# WHAT IS ASSERTED (ASan+UBSan):
#   1. unarmed, file_list returns the three names, none NULL;
#   2. for k = 1, 2, 3: file_list returns is_err, kind FILE_ERR_IO, message
#      "allocation failed" (ASan would report a double free or a use of a
#      freed entry on the cleanup path);
#   3. through the glue, with k = 2: tk_file_list_w and tk_file_listdir_w
#      return 0 with the error slot set, never an array holding a NULL;
#   4. after the injected failures the next unarmed call succeeds again.
#
# Negative control: C1650_STDLIB=<dir> uses another tree's src/stdlib.
#   Base e810734c: 2 passed, 3 failed (cases 2 and 3 for both glue calls: the
#   call succeeds with a NULL element).
# Measured: this branch 5 passed.
#
# Story: 127.414
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1650_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1650_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1650: file_list fails cleanly when strdup fails (127.414)"
echo "-----------------------------------------------------------"

mkdir -p d
printf 'A' > d/n1
printf 'B' > d/n2
printf 'C' > d/n3

cat > inj.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "file.h"
#include "capabilities.h"
#include "tk_runtime.h"   /* tk_current_error (thread-local) */
int64_t tk_file_list_w(int64_t dir);
int64_t tk_file_listdir_w(int64_t dir);

static int armed = 0, countdown = 0;
char *strdup(const char *s)
{
    if (armed && (strcmp(s, "n1") == 0 || strcmp(s, "n2") == 0 || strcmp(s, "n3") == 0)
        && --countdown == 0)
        return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)

static int list_ok(void)
{
    StrArrayFileResult r = file_list("d");
    if (r.is_err || r.ok.len != 3) { printf("        (is_err %d, len %llu)\n", r.is_err,
                                            (unsigned long long)r.ok.len); return 0; }
    int ok = 1;
    for (uint64_t i = 0; i < r.ok.len; i++) {
        if (!r.ok.data[i]) { printf("        ([%llu] NULL)\n", (unsigned long long)i); ok = 0; }
        else free((void *)r.ok.data[i]);
    }
    free(r.ok.data);
    return ok;
}

/* With the second fixture strdup failing, the glue must return 0 with the
 * error slot set; the base returned an array holding a NULL element. */
static int glue_fails(int64_t (*fn)(int64_t), const char *name)
{
    tk_current_error = 0;
    armed = 1; countdown = 2;
    int64_t a = fn((int64_t)(intptr_t)"d");
    armed = 0;
    if (a == 0 && tk_current_error != 0) return 1;
    if (a) {
        int64_t n = ((int64_t *)(intptr_t)a)[-1];
        for (int64_t i = 0; i < n; i++)
            if (!((int64_t *)(intptr_t)a)[i])
                printf("        (%s: succeeded, element %lld is NULL)\n", name, (long long)i);
    } else printf("        (%s: 0 but the error slot is clear)\n", name);
    return 0;
}

int main(void)
{
    char a0[] = "inj", a1[] = "--allow-read";
    char *cargv[] = { a0, a1, NULL };
    tk_cap_init(2, cargv);              /* the wrappers check fs.read */
    setvbuf(stdout, NULL, _IONBF, 0);   /* every line out before an abort */
    CHECK(list_ok(), "unarmed: file_list returns the three names, none NULL");

    int all = 1;
    for (int k = 1; k <= 3; k++) {
        armed = 1; countdown = k;
        StrArrayFileResult r = file_list("d");
        armed = 0;
        int ok = r.is_err && r.err.kind == FILE_ERR_IO && r.err.msg
                 && strcmp(r.err.msg, "allocation failed") == 0;
        if (!ok) {
            printf("        (k=%d: is_err %d, len %llu)\n", k, r.is_err, (unsigned long long)r.ok.len);
            all = 0;
            if (!r.is_err) {
                for (uint64_t i = 0; i < r.ok.len; i++) free((void *)r.ok.data[i]);
                free(r.ok.data);
            }
        }
    }
    CHECK(all, "k=1..3: file_list fails with FILE_ERR_IO \"allocation failed\"");

    CHECK(glue_fails(tk_file_list_w, "file.list"), "glue: tk_file_list_w raises, no NULL element");
    CHECK(glue_fails(tk_file_listdir_w, "file.listdir"), "glue: tk_file_listdir_w raises, no NULL element");

    CHECK(list_ok(), "after the injected failures the next unarmed call succeeds");
    return bad ? 1 : 0;
}
C
# The link set comes from scripts/stdlib_link_set.sh as cached objects built
# with these flags, never a hand list.
# shellcheck disable=SC2206
CF=(-std=c99 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects file -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o inj inj.c "${LS[@]}" -lz -lpthread -lm > inj.cc 2>&1; then
    fail "harness compile"; head -20 inj.cc
else
    TK_STR_VERIFY=1 ./inj > inj.out 2> inj.err
    rc=$?
    cat inj.out
    p=$(grep -c '^  PASS:' inj.out); f=$(grep -c '^  FAIL:' inj.out)
    PASS=$((PASS + p)); FAIL=$((FAIL + f))
    if [ "${rc}" -ne 0 ] && [ "${f}" -eq 0 ]; then
        fail "harness exited ${rc} (sanitizer?)"; head -30 inj.err
    fi
fi

echo "-----------------------------------------------------------"
echo "C1650: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
