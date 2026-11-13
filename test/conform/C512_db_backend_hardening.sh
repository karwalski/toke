#!/usr/bin/env bash
# C512_db_backend_hardening.sh -- std.db's SQLite backend does not leak or
# ignore driver errors (story 121.21; audit 120.10 DAT-08).
#
# 121.71 archived db_mysql.c and db_postgres.c (never dispatched by any
# build: db.c talks to SQLite directly), and with them this test's MySQL
# (DAT-03, DAT-04, DAT-06) and Postgres (DAT-06) harnesses; the full
# original test is in the archive beside them.  The SQLite cases remain.
#
# WHAT WAS WRONG (read at main e1cef7da, and each case below is red there):
#   DAT-08  db.c exec_simple (BEGIN/COMMIT/ROLLBACK) kept sqlite3_exec's
#           errmsg in the DbErr and never sqlite3_free'd it.
#   and     db_many ignored sqlite3_bind_text's result, so surplus parameters
#           (SQLITE_RANGE) ran the query anyway; db_one refused them.
#
# HOW.  SQLite is real; the build is -fsanitize=address,undefined
# -fno-sanitize-recover.
#
# Negative control: C512_STDLIB=<dir> builds against another src/stdlib
# tree.  main's tree fails every non-control case.
#
# Story: 121.21
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${C512_STDLIB:-${REPO_ROOT}/src/stdlib}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c512_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT
PASS=0; FAIL=0; SKIP=0
pass() { echo "  PASS: $1"; PASS=$((PASS + 1)); }
fail() { echo "  FAIL: $1"; FAIL=$((FAIL + 1)); }
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0"
collect() {  # $1 output file: fold its PASS/FAIL lines into the totals
    /usr/bin/grep -a -E '^  (PASS|FAIL):' "$1"
    PASS=$((PASS + $(/usr/bin/grep -a -c '^  PASS:' "$1")))
    FAIL=$((FAIL + $(/usr/bin/grep -a -c '^  FAIL:' "$1")))
    if ! /usr/bin/grep -a -q '^HARNESS' "$1"; then
        fail "$2 harness did not finish (sanitizer abort?)"; tail -12 "$1" | sed 's/^/      | /'
    fi
}
CHECK_H='
#include <stdio.h>
static int fails = 0;
#define CHECK(c, ...) do { if (c) { printf("  PASS: "); printf(__VA_ARGS__); printf("\n"); } \
                           else { printf("  FAIL: "); printf(__VA_ARGS__); printf("\n"); fails++; } } while (0)
#include <stdint.h>
'

echo "C512: std.db SQLite backend -- driver errors are freed and not ignored (121.21)"
echo "-------------------------------------------------------------------------------"

# ── SQLite, real ───────────────────────────────────────────────────────────
# db.c's sqlite3_exec/sqlite3_free are counted through a forced include
# (sqlite3_memory_used cannot be trusted: a system SQLite may be built with
# memory statistics off, and then it reads 0 whatever leaks).
cat > "${WORK}/sqhook.h" <<'H'
#include <sqlite3.h>
int  c512_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **), void *arg, char **err);
void c512_free(void *p);
#define sqlite3_exec c512_exec
#define sqlite3_free c512_free
H
cat > "${WORK}/tsq.c" <<C
#include <string.h>
#include <sqlite3.h>
#include "db.h"
#include "capabilities.h"
${CHECK_H}
static int g_errmsgs = 0, g_freed = 0;
static void *g_last[64]; static int g_nlast = 0;
int c512_exec(sqlite3 *db, const char *sql, int (*cb)(void *, int, char **, char **), void *arg, char **err) {
    int rc = sqlite3_exec(db, sql, cb, arg, err);
    if (err && *err) { g_errmsgs++; if (g_nlast < 64) g_last[g_nlast++] = *err; }
    return rc;
}
void c512_free(void *p) {
    for (int i = 0; i < g_nlast; i++) if (p && g_last[i] == p) { g_freed++; g_last[i] = NULL; }
    sqlite3_free(p);
}
int main(void) {
    char *av[] = {"t", "--allow-all", NULL}; tk_cap_init(2, av);
    if (db_connect(":memory:") <= 0) { printf("  FAIL: open :memory:\n"); return 1; }
    StrArray none = { NULL, 0 };
    (void)db_exec("CREATE TABLE t(a TEXT)", none);

    /* DAT-08: a failing COMMIT (no transaction) returns its message, and
     * every errmsg sqlite3_exec hands db.c is given back with sqlite3_free. */
    BoolResult c0 = db_commit(0);
    CHECK(c0.is_err && c0.err.msg && strstr(c0.err.msg, "no transaction"),
          "DAT-08 a failing COMMIT reports sqlite's message (%s)", c0.err.msg ? c0.err.msg : "(null)");
    for (int i = 0; i < 50; i++) (void)db_commit(0);
    CHECK(g_errmsgs == 51 && g_freed == g_errmsgs,
          "DAT-08 51 failing COMMITs: %d errmsg allocations, %d sqlite3_free'd", g_errmsgs, g_freed);

    /* surplus parameters: db.many refuses them, like db.one */
    const char *two[2] = { "a", "b" }; StrArray p2 = { two, 2 };
    RowResult o = db_one("SELECT ?", p2);
    RowArrayResult m = db_many("SELECT ?", p2);
    CHECK(o.is_err, "control: db.one with 2 parameters for 1 placeholder is an error");
    CHECK(m.is_err && m.err.kind == DB_ERR_QUERY,
          "db.many with 2 parameters for 1 placeholder is an error too (got %s)",
          m.is_err ? m.err.msg : "rows");
    const char *one1[1] = { "z" }; StrArray p1 = { one1, 1 };
    RowArrayResult ok = db_many("SELECT ? AS v", p1);
    CHECK(!ok.is_err && ok.ok.len == 1 && !strcmp(ok.ok.data[0].col_values[0], "z"),
          "control: db.many with matching parameters returns the bound value");
    printf("HARNESS %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
C
# 127.295 (Z193): db.c's err_copy seals its copy (tks_strdup, str.c), so
# db.c no longer links alone.  The rest of std.db's link set comes from
# stdlib_link_set.sh --objects; db.c is left out of it (--except) because it
# is compiled here with the sqlite3_exec/sqlite3_free hook forced in.  The
# real capabilities.c is in the set, so the harness grants --allow-all
# rather than stubbing tk_cap_check.
# shellcheck disable=SC2206
CF=(-std=c11 -D_GNU_SOURCE ${SAN} -iquote "${S}" -I"${S}")
TKC="${TKC:-${REPO_ROOT}/tkc}"
LINKSET="$("${REPO_ROOT}/scripts/stdlib_link_set.sh" --stdlib "${S}" --tkc "${TKC}" --cc "${CC}" \
        --except db.c --objects db -- "${CF[@]}")" \
    || { fail "stdlib link set: see stderr above"; LINKSET=""; }
LS=(); while IFS= read -r l; do [ -n "$l" ] && LS+=("$l"); done <<< "${LINKSET}"
if [ -n "${LINKSET}" ] \
   && "${CC}" "${CF[@]}" -c -o "${WORK}/db.o" -include "${WORK}/sqhook.h" \
        "${S}/db.c" >"${WORK}/sq.cc" 2>&1 \
   && "${CC}" "${CF[@]}" -o "${WORK}/tsq" "${WORK}/tsq.c" "${WORK}/db.o" "${LS[@]}" \
        -lsqlite3 -lz -lpthread -lm >>"${WORK}/sq.cc" 2>&1; then
    "${WORK}/tsq" >"${WORK}/tsq.out" 2>&1; collect "${WORK}/tsq.out" sqlite
else
    fail "sqlite harness did not build"; head -15 "${WORK}/sq.cc" | sed 's/^/      | /'
fi

echo "-------------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
[ "${FAIL}" -eq 0 ]
