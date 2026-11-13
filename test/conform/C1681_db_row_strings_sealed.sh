#!/usr/bin/env bash
# C1681_db_row_strings_sealed.sh -- every column name and value of a $dbrow
# that db.one / db.many hand toke is a sealed string (story 127.295, S1 step
# 5 final tranche; runtime-abi §3.1).
#
# db.c's collect_row strdup's each name and value (its own error paths and C
# callers free them with free_row); row_rec in db_glue.c used to put those
# malloc'd pointers straight into the $dbrow's cols.  It now puts sealed
# copies there and frees the originals it copied, so the caller still drops
# only the Row's index.  On failure the Row is left whole for
# rows_free_from, as before.
#
# WHAT IS ASSERTED (C driver, ASan+UBSan, TK_STR_VERIFY=1):
#   1. db.one: every name and value in .cols is sealed (header == strlen),
#      the values are right, SQL NULL is a sealed ""
#   2. db.many: every name and value of every row is sealed
#   3. row.str on a queried row returns the value
# and a toke program reads r.cols / row.str / .len under TK_STR_VERIFY=1 (a
# db.many row's pair is annotated @(str): unannotated, its .get(1)
# interpolates as an integer on main too -- story 127.424).
#
# Negative control: C1681_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).
#   Base c3051e18's src/stdlib + stdlib: 2 passed, 2 failed -- db.one and
#   db.many report "not sealed"; row.str and the toke program pass.
# Measured: this branch 4 passed (3 C + 1 toke).
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1681_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1681_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1681: std.db \$dbrow strings sealed (127.295)"
echo "----------------------------------------------"

cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "capabilities.h"
#include "tk_runtime.h"
int64_t tk_db_open_w(int64_t);
int64_t tk_db_exec_w(int64_t, int64_t);
int64_t tk_db_one_w(int64_t, int64_t);
int64_t tk_db_manyon_w(int64_t, int64_t, int64_t);
int64_t tk_row_str_w(int64_t, int64_t);
static int bad = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); bad++; } } while (0)
#define I(p) ((int64_t)(intptr_t)(p))
#define P(v) ((const char *)(intptr_t)(v))
static int64_t strarr(const char **v, int n) {   /* a toke [str]: len at [-1] */
    int64_t *b = calloc((size_t)n + 1, sizeof(int64_t));
    b[0] = n; for (int i = 0; i < n; i++) b[i + 1] = I(v[i]);
    return I(b + 1);
}
static int sealed(const char *s)
{
    if (!s) { printf("        (NULL)\n"); return 0; }
    int64_t h = tks_hdrlen(s);
    if (h < 0) { printf("        (not sealed: '%.40s')\n", s); return 0; }
    return (size_t)h == strlen(s);
}
/* $dbrow = {cols:[[str]]}: slot 0 is the cols array, each a @(name; value). */
static int row_sealed(int64_t rec, const char *const *want, int n)
{
    if (!rec) { printf("        (no row)\n"); return 0; }
    int64_t *cols = (int64_t *)(intptr_t)((int64_t *)(intptr_t)rec)[0];
    if (cols[-1] != n) { printf("        (%lld cols)\n", (long long)cols[-1]); return 0; }
    int ok = 1;
    for (int c = 0; c < n; c++) {
        int64_t *pair = (int64_t *)(intptr_t)cols[c];
        const char *nm = P(pair[0]), *v = P(pair[1]);
        ok &= sealed(nm) && sealed(v);
        if (want && strcmp(v, want[c]) != 0) { printf("        (col %d = '%s')\n", c, v); ok = 0; }
    }
    return ok;
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    int64_t none = strarr(NULL, 0);
    int64_t h = tk_db_open_w(I("c1681.db"));
    tk_db_exec_w(I("CREATE TABLE t(a TEXT, b INTEGER, c TEXT)"), none);
    const char *v1[] = { "hello", "42" };
    tk_db_exec_w(I("INSERT INTO t VALUES(?, ?, NULL)"), strarr(v1, 2));
    const char *v2[] = { "bye", "7", "x" };
    tk_db_exec_w(I("INSERT INTO t VALUES(?, ?, ?)"), strarr(v2, 3));
    const char *w1[] = { "hello", "42", "" };
    int64_t r = tk_db_one_w(I("SELECT a, b, c FROM t WHERE b=42"), none);
    CHECK(tk_current_error == 0 && row_sealed(r, w1, 3),
          "db.one: every name and value sealed; SQL NULL a sealed \"\"");
    int64_t rs = tk_db_manyon_w(h, I("SELECT a, b, c FROM t ORDER BY b"), none);
    int ok = tk_current_error == 0 && rs && ((int64_t *)(intptr_t)rs)[-1] == 2;
    const char *m0[] = { "bye", "7", "x" };
    for (int i = 0; ok && i < 2; i++)
        ok &= row_sealed(((int64_t *)(intptr_t)rs)[i], i == 0 ? m0 : w1, 3);
    CHECK(ok, "db.many: every name and value of every row sealed");
    int64_t a = tk_row_str_w(r, I("a"));
    CHECK(a && strcmp(P(a), "hello") == 0, "row.str on a queried row");
    return bad ? 1 : 0;
}
C
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -I"${S}")
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --objects db -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -z "${LINKSET}" ]; then
    :
elif ! "${CC}" "${CF[@]}" -o seal seal.c "${LS[@]}" -lsqlite3 -lz -lpthread -lm > seal.cc 2>&1; then
    fail "C harness did not build"; sed 's/^/        /' seal.cc | head -6
else
    TK_STR_VERIFY=1 ./seal > seal.out 2>&1; rc=$?
    grep -E '^  (PASS|FAIL)|^        ' seal.out | head -40
    PASS=$((PASS + $(grep -c '^  PASS' seal.out))); FAIL=$((FAIL + $(grep -c '^  FAIL' seal.out)))
    if [ "${rc}" -ne 0 ] && grep -a -q -E 'ERROR: AddressSanitizer|runtime error' seal.out; then
        fail "C harness stopped: $(grep -a -m1 -o -E 'AddressSanitizer: [a-z-]+|runtime error: .{0,60}' seal.out)"
    elif [ "${rc}" -ne 0 ] && ! grep -q '^  FAIL' seal.out; then
        fail "C harness exited ${rc}"; tail -8 seal.out | sed 's/^/        /'
    fi
fi

cat > p.tk <<'TOKE'
m=p;
i=io:std.io;
i=str:std.str;
i=db:std.db;
f=main():i64{
  let h=mt db.open("p.db") {$ok:v v;$err:e 0};
  let x=db.exec("CREATE TABLE t(a TEXT, b TEXT)";@());
  let y=db.exec("INSERT INTO t VALUES(?, ?)";@("tok";"en"));
  let r=mt db.one("SELECT a, b FROM t";@()) {$ok:v v;$err:e $dbrow{cols:@()}};
  let p=r.cols.get(1);
  let a=mt row.str(r;"a") {$ok:v v;$err:e "err"};
  io.println("\(p.get(0))=\(p.get(1)) \(p.get(1).len) \(a) \(a.len)");
  let rs=mt db.many("SELECT a, b FROM t";@()) {$ok:v v;$err:e @()};
  let q:@(str)=rs.get(0).cols.get(0);
  io.println("\(rs.len) \(q.get(1))");
  <0
};
TOKE
WANT='b=en 2 tok 3
1 tok'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -8 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: \$dbrow cols, row.str and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "----------------------------------------------"
echo "C1681: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
