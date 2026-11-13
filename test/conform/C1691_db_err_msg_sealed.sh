#!/usr/bin/env bash
# C1691_db_err_msg_sealed.sh -- the `e.msg` of every std.db error a program
# can bind is a sealed string (story 127.295, S1 step 5: the db.c err_copy
# item of runtime-abi §3.1's end state).
#
# WHAT WAS WRONG (base 586185a5).  db_glue.c's db_raise put DbErr.msg
# straight into the {tag, msg} error box.  That pointer was db.c's err_copy
# strdup (sqlite's message, g_errbuf), a C literal ("no rows returned",
# "db.exec: null sql", "not a connection handle") or row.*'s column name --
# none sealed, so every reader of `e.msg` fell back to strlen.
#
# THE FIX.  err_copy copies with tks_strdup (so db.c now links str.c: C512
# and test-stdlib-db name it), and db_raise gives anything still unsealed a
# sealed copy.  Both copies are never freed, as before.
#
# WHAT IS ASSERTED (C driver, ASan+UBSan, TK_STR_VERIFY=1):
#   1. db.c alone: db_one's error for bad SQL carries a sealed err.msg
#   2. glue: e.msg sealed and right for a sqlite error (db.exec bad SQL), a
#      literal (db.one on an empty table: "no rows returned"), a handle error
#      (db.execon(0; ..)), and row.str of a missing column (the name)
#   3. a toke program prints e.msg and e.msg.len under TK_STR_VERIFY=1
#
# Negative control: C1691_STDLIB=<dir> uses another tree's src/stdlib (the
# tree's ../../stdlib must hold its .tki files).  Measured: base 586185a5's
# src/stdlib 2 passed, 5 failed (1 and the four glue cases: "not sealed";
# the row control and the toke program pass); this branch 7 passed.
#
# Story: 127.295
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
TKC="${TKC:-${REPO_ROOT}/tkc}"
S="${C1691_STDLIB:-${REPO_ROOT}/src/stdlib}"
export TKC_STDLIB_DIR="${S}"
CC="${CC:-cc}"
PASS=0; FAIL=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
if [ ! -x "${TKC}" ]; then echo "FAIL: tkc binary not found at ${TKC} (run 'make' first)"; exit 1; fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c1691_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
cd "${WORK}" || exit 1
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
echo "C1691: std.db error e.msg sealed (127.295)"
echo "------------------------------------------"

cat > seal.c <<'C'
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "tk_strhdr.h"
#include "capabilities.h"
#include "tk_runtime.h"
#include "db.h"
int64_t tk_db_open_w(int64_t);
int64_t tk_db_exec_w(int64_t, int64_t);
int64_t tk_db_execon_w(int64_t, int64_t, int64_t);
int64_t tk_db_one_w(int64_t, int64_t);
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
    if (h < 0) { printf("        (not sealed: '%.50s')\n", s); return 0; }
    return (size_t)h == strlen(s);
}
/* the {tag, msg} box db_raise leaves in tk_current_error */
static int raised(int64_t tag, const char *want)
{
    if (!tk_current_error) { printf("        (no error raised)\n"); return 0; }
    int64_t *box = (int64_t *)(intptr_t)tk_current_error;
    const char *m = P(box[1]);
    if (box[0] != tag) { printf("        (tag %lld)\n", (long long)box[0]); return 0; }
    if (!m || !strstr(m, want)) { printf("        (msg '%s')\n", m ? m : "(null)"); return 0; }
    return sealed(m);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    int64_t none = strarr(NULL, 0);
    int64_t h = tk_db_open_w(I("c1691.db"));
    tk_db_exec_w(I("CREATE TABLE t(a TEXT)"), none);

    StrArray sn = { NULL, 0 };
    RowResult r0 = db_one("SELEC nonsense", sn);
    CHECK(r0.is_err && r0.err.msg && strstr(r0.err.msg, "syntax") && sealed(r0.err.msg),
          "db.c db_one: the error's msg (err_copy of sqlite's text) is sealed");

    tk_db_exec_w(I("INSERT INTO nosuch VALUES(1)"), none);
    CHECK(raised(DB_ERR_QUERY, "no such table"), "db.exec: sqlite's message, sealed");
    tk_db_one_w(I("SELECT a FROM t"), none);
    CHECK(raised(DB_ERR_NOT_FOUND, "no rows returned"), "db.one on no rows: the literal, sealed");
    tk_db_execon_w(0, I("SELECT 1"), none);
    CHECK(raised(DB_ERR_CONNECTION, "not a connection handle"), "db.execon(0;..): the literal, sealed");
    tk_db_exec_w(I("INSERT INTO t VALUES('v')"), none);
    int64_t row = tk_db_one_w(I("SELECT a FROM t"), none);
    CHECK(row && tk_current_error == 0, "control: db.one returns the row");
    tk_row_str_w(row, I("nocol"));
    CHECK(raised(DB_ERR_NOT_FOUND, "nocol"), "row.str of a missing column: the name, sealed");
    (void)h;
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
  let x=db.exec("CREATE TABLE t(a TEXT)";@());
  let a=mt db.exec("INSERT INTO nosuch VALUES(1)";@()) {$ok:v "ok";$err:e mt e {$connection:m m;$query:m m;$notfound:m m;$constraint:m m}};
  let b=mt db.one("SELECT a FROM t";@()) {$ok:v "ok";$err:e mt e {$connection:m m;$query:m m;$notfound:m m;$constraint:m m}};
  io.println("\(a) \(a.len)");
  io.println("\(b) \(b.len)");
  <0
};
TOKE
WANT='db.exec: statement 1: no such table: nosuch 43
no rows returned 16'
if ! "${TKC}" --allow-all --out p p.tk > p.log 2>&1; then
    fail "toke: compile"; head -c 600 p.log | sed 's/^/        /'
else
    got="$(TK_STR_VERIFY=1 ./p 2>&1)"
    if [ "${got}" = "${WANT}" ]; then
        pass "toke: e's message and .len under TK_STR_VERIFY=1"
    else
        fail "toke: output differs"; printf '%s\n' "${got}" | head -8 | sed 's/^/        /'
    fi
fi

echo "------------------------------------------"
echo "C1691: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
