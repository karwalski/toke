#!/usr/bin/env bash
# C542_db_error_text_owned.sh — a db error's e.msg owns its text: it survives
# later db calls and db.close (story 121.51 item 5).
#
# THE DEFECT, measured on main (d3a083b8) + fix/r2-rsa:
#   db.c put sqlite3_errmsg()'s pointer, or its thread-local g_errbuf, into
#   DbErr.msg, and db_glue.c's error box carried that pointer to toke.  The
#   next failing call overwrote either buffer, and sqlite3_close() frees
#   sqlite's.  Through the glue: e.msg from db.one/db.exec/db.many named the
#   LATER error's column (3 wrong), and e.msg read after db.close was an ASan
#   heap-use-after-free.  db_postgres.c (PQerrorMessage) and db_mysql.c
#   (mysql_error / mysql_stmt_error) did the same; they get the same copy but
#   are not driven here (no server).
#
# NOW: every non-literal message is copied (err_copy / pg_err_copy /
# my_err_copy).  This drives the glue under ASan+UBSan.
#
# Negative control: C542_STDLIB=<dir> builds against another src/stdlib tree;
# the pre-fix tree fails three checks and then aborts in ASan on the fourth.
#
# Story: 121.51
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C542_STDLIB:-${REPO_ROOT}/src/stdlib}"
PASS=0; FAIL=0
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c542_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT; cd "${WORK}"
echo "C542: db error text survives later db calls and db.close (121.51)"
echo "-----------------------------------------------------------------"

cat > t.c <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include "db.h"
#include "capabilities.h"
#include "tk_runtime.h"
int64_t tk_db_open_w(int64_t);
int64_t tk_db_close_w(void);
int64_t tk_db_exec_w(int64_t, int64_t);
int64_t tk_db_one_w(int64_t, int64_t);
int64_t tk_db_many_w(int64_t, int64_t);
int64_t tk_time_nowms_w(void) { return 0; }
static int fails = 0;
#define CHECK(c, m) do { if (c) printf("  PASS: %s\n", m); \
                         else { printf("  FAIL: %s\n", m); fails++; } } while (0)
#define S(x) ((int64_t)(intptr_t)(x))
static int64_t strarr(int n) { int64_t *b = calloc((size_t)n + 1, sizeof(int64_t)); b[0] = n; return S(b + 1); }
static const char *slot_msg(void) {
    int64_t *box = (int64_t *)(intptr_t)tk_current_error;
    return box ? (const char *)(intptr_t)box[1] : NULL;
}
int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    int64_t none = strarr(0);
    tk_db_open_w(S(":memory:"));
    tk_db_exec_w(S("CREATE TABLE t(v INTEGER)"), none);
    /* one: sqlite3_errmsg */
    tk_db_one_w(S("SELECT nope FROM t"), none);        const char *m1 = slot_msg();
    /* exec: the composed buffer */
    tk_db_exec_w(S("SELECT alpha FROM t"), none);      const char *m2 = slot_msg();
    /* many: sqlite3_errmsg */
    tk_db_many_w(S("SELECT beta FROM t"), none);       const char *m3 = slot_msg();
    tk_db_exec_w(S("SELECT gamma FROM zz"), none);     /* a later error */
    CHECK(m1 && strstr(m1, "nope"),  "db.one's e.msg still names its own column after later errors");
    CHECK(m2 && strstr(m2, "alpha"), "db.exec's e.msg still names its own column after later errors");
    CHECK(m3 && strstr(m3, "beta"),  "db.many's e.msg still names its own column after later errors");
    tk_db_one_w(S("SELECT delta FROM t"), none);       const char *m4 = slot_msg();
    tk_db_close_w();
    CHECK(m4 && strstr(m4, "delta"), "db.one's e.msg is readable after db.close");
    printf("C-driver: %d failure(s)\n", fails);
    return fails ? 1 : 0;
}
C
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# shellcheck disable=SC2086
if ! "${CC}" -std=c11 -D_GNU_SOURCE -g -O1 ${SAN} -iquote "${S}" -o t t.c "${S}/db.c" "${S}/db_glue.c" \
        "${S}/capabilities.c" "${S}/tk_runtime.c" "${S}/args.c" -lsqlite3 > cc.log 2>&1; then
    echo "  FAIL: ASan build: $(head -c 600 cc.log)"; FAIL=$((FAIL + 1))
else
    ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" ./t > t.out 2>&1
    rc=$?
    /usr/bin/grep -a '^  PASS\|^  FAIL' t.out
    PASS=$((PASS + $(/usr/bin/grep -a -c '^  PASS' t.out)))
    FAIL=$((FAIL + $(/usr/bin/grep -a -c '^  FAIL' t.out)))
    if /usr/bin/grep -a -q 'AddressSanitizer\|runtime error' t.out; then
        echo "  FAIL: sanitizer report: $(/usr/bin/grep -a -m1 'AddressSanitizer\|runtime error' t.out)"
        FAIL=$((FAIL + 1))
    elif [ "${rc}" -ne 0 ] && [ "$(/usr/bin/grep -a -c '^  FAIL' t.out)" -eq 0 ]; then
        echo "  FAIL: driver exit ${rc}: $(tail -1 t.out)"; FAIL=$((FAIL + 1))
    fi
fi
echo ""
echo "C542: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
