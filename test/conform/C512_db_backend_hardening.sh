#!/usr/bin/env bash
# C512_db_backend_hardening.sh -- std.db's backends bind every parameter,
# require TLS by default, and do not leak or ignore driver errors
# (story 121.21; audit 120.10 DAT-03, DAT-04, DAT-06, DAT-08).
#
# WHAT WAS WRONG (read at main e1cef7da, and each case below is red there):
#   DAT-03  db_mysql.c my_one/my_many/my_exec did `(void)params` and sent the
#           SQL text alone through mysql_query(): the `?`s were never bound.
#   DAT-04  my_table_exists formatted the name into the SQL between quotes.
#   DAT-06  my_open set no SSL option (plaintext fallback, credentials
#           included); pg_open used PQconnectdb(dsn), libpq's default sslmode
#           "prefer" -- plaintext whenever the server offers no TLS.
#   DAT-08  db.c exec_simple (BEGIN/COMMIT/ROLLBACK) kept sqlite3_exec's
#           errmsg in the DbErr and never sqlite3_free'd it.
#   and     db_many ignored sqlite3_bind_text's result, so surplus parameters
#           (SQLITE_RANGE) ran the query anyway; db_one refused them.
#
# HOW.  The MySQL backend is not dispatched by any shipped build and this
# host has no libmysqlclient, so it is compiled (-DTK_HAVE_MYSQL) against a
# recording mock of the client API written below: the mock logs every
# mysql_query, mysql_stmt_prepare, bound parameter, execute and option, and
# the checks read that log.  Postgres is compiled against the real libpq
# (skipped, and said so, when pkg-config cannot find it) with
# PQconnectdb/PQconnectdbParams routed through a hook that asks libpq, via
# PQconninfo, which sslmode the connection actually used.  SQLite is real.
# Every build is -fsanitize=address,undefined -fno-sanitize-recover.
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
int  tk_cap_check(uint32_t cap) { (void)cap; return 1; }
void tk_cap_deny(uint32_t cap)  { (void)cap; }
'

echo "C512: std.db backends -- bound parameters, TLS by default, driver errors (121.21)"
echo "-------------------------------------------------------------------------------"

# ── MySQL, against a recording mock ────────────────────────────────────────
mkdir -p "${WORK}/mock/mysql"
cat > "${WORK}/mock/mysql/mysql.h" <<'H'
/* Recording mock of the libmysqlclient subset db_mysql.c uses (C512). */
#include <stdint.h>
typedef char my_bool;
typedef unsigned long long my_ulonglong;
typedef struct MYSQL { int unused; } MYSQL;
typedef char **MYSQL_ROW;
typedef struct { char *name; unsigned long length; } MYSQL_FIELD;
typedef struct MYSQL_RES { int from_stmt; } MYSQL_RES;
enum enum_field_types { MYSQL_TYPE_STRING = 254 };
typedef struct { enum enum_field_types buffer_type; void *buffer; unsigned long buffer_length;
                 unsigned long *length; my_bool *is_null; } MYSQL_BIND;
typedef struct MYSQL_STMT MYSQL_STMT;
enum mysql_option { MYSQL_OPT_SSL_MODE = 42 };
enum mysql_ssl_mode { SSL_MODE_DISABLED = 1, SSL_MODE_PREFERRED, SSL_MODE_REQUIRED };
#define MYSQL_NO_DATA 100
#define MYSQL_DATA_TRUNCATED 101
MYSQL *mysql_init(MYSQL *m);
int mysql_options(MYSQL *m, enum mysql_option o, const void *arg);
MYSQL *mysql_real_connect(MYSQL *m, const char *h, const char *u, const char *p, const char *d,
                          unsigned int port, const char *sock, unsigned long flags);
void mysql_close(MYSQL *m);
int mysql_query(MYSQL *m, const char *q);
const char *mysql_error(MYSQL *m);
MYSQL_RES *mysql_store_result(MYSQL *m);
MYSQL_ROW mysql_fetch_row(MYSQL_RES *r);
void mysql_free_result(MYSQL_RES *r);
unsigned int mysql_num_fields(MYSQL_RES *r);
MYSQL_FIELD *mysql_fetch_fields(MYSQL_RES *r);
unsigned long *mysql_fetch_lengths(MYSQL_RES *r);
my_ulonglong mysql_num_rows(MYSQL_RES *r);
my_ulonglong mysql_affected_rows(MYSQL *m);
my_bool mysql_commit(MYSQL *m);
my_bool mysql_rollback(MYSQL *m);
my_ulonglong mysql_insert_id(MYSQL *m);
MYSQL_STMT *mysql_stmt_init(MYSQL *m);
int mysql_stmt_prepare(MYSQL_STMT *s, const char *q, unsigned long len);
const char *mysql_stmt_error(MYSQL_STMT *s);
my_bool mysql_stmt_close(MYSQL_STMT *s);
MYSQL_RES *mysql_stmt_result_metadata(MYSQL_STMT *s);
my_bool mysql_stmt_bind_result(MYSQL_STMT *s, MYSQL_BIND *b);
my_bool mysql_stmt_bind_param(MYSQL_STMT *s, MYSQL_BIND *b);
int mysql_stmt_execute(MYSQL_STMT *s);
int mysql_stmt_fetch(MYSQL_STMT *s);
unsigned long mysql_stmt_param_count(MYSQL_STMT *s);
my_ulonglong mysql_stmt_affected_rows(MYSQL_STMT *s);
H
cat > "${WORK}/mock.c" <<'C'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "mysql/mysql.h"
char g_log[64][512]; int g_nlog = 0;
static void logf_(const char *k, const char *v) {
    if (g_nlog < 64) snprintf(g_log[g_nlog++], 512, "%s:%s", k, v ? v : "");
}
struct MYSQL_STMT { unsigned long nparams; char params[8][256]; MYSQL_BIND *res; int fetched;
                    int is_select; MYSQL_RES meta; };
static MYSQL the_conn;
static MYSQL_FIELD field = { "v", 255 };
MYSQL *mysql_init(MYSQL *m) { (void)m; return &the_conn; }
int mysql_options(MYSQL *m, enum mysql_option o, const void *arg) {
    (void)m; char b[64]; snprintf(b, sizeof b, "%d=%u", (int)o, *(const unsigned int *)arg);
    logf_("option", b); return 0; }
MYSQL *mysql_real_connect(MYSQL *m, const char *h, const char *u, const char *p, const char *d,
                          unsigned int port, const char *sock, unsigned long flags) {
    (void)h; (void)u; (void)p; (void)d; (void)port; (void)sock; (void)flags;
    logf_("connect", ""); return m; }
void mysql_close(MYSQL *m) { (void)m; }
int mysql_query(MYSQL *m, const char *q) { (void)m; logf_("query", q); return 0; }
const char *mysql_error(MYSQL *m) { (void)m; return "mock: no result set"; }
MYSQL_RES *mysql_store_result(MYSQL *m) { (void)m; return NULL; }
MYSQL_ROW mysql_fetch_row(MYSQL_RES *r) { (void)r; return NULL; }
void mysql_free_result(MYSQL_RES *r) { (void)r; }
unsigned int mysql_num_fields(MYSQL_RES *r) { (void)r; return 1; }
MYSQL_FIELD *mysql_fetch_fields(MYSQL_RES *r) { (void)r; return &field; }
unsigned long *mysql_fetch_lengths(MYSQL_RES *r) { (void)r; return NULL; }
my_ulonglong mysql_num_rows(MYSQL_RES *r) { (void)r; return 0; }
my_ulonglong mysql_affected_rows(MYSQL *m) { (void)m; return 0; }
my_bool mysql_commit(MYSQL *m) { (void)m; return 0; }
my_bool mysql_rollback(MYSQL *m) { (void)m; return 0; }
my_ulonglong mysql_insert_id(MYSQL *m) { (void)m; return 0; }
MYSQL_STMT *mysql_stmt_init(MYSQL *m) { (void)m; return calloc(1, sizeof(MYSQL_STMT)); }
int mysql_stmt_prepare(MYSQL_STMT *s, const char *q, unsigned long len) {
    (void)len; logf_("prepare", q);
    for (const char *p = q; *p; p++) if (*p == '?') s->nparams++;
    s->is_select = !strncmp(q, "SELECT", 6); return 0; }
const char *mysql_stmt_error(MYSQL_STMT *s) { (void)s; return "mock stmt error"; }
my_bool mysql_stmt_close(MYSQL_STMT *s) { free(s); return 0; }
MYSQL_RES *mysql_stmt_result_metadata(MYSQL_STMT *s) { return s->is_select ? &s->meta : NULL; }
my_bool mysql_stmt_bind_result(MYSQL_STMT *s, MYSQL_BIND *b) { s->res = b; return 0; }
my_bool mysql_stmt_bind_param(MYSQL_STMT *s, MYSQL_BIND *b) {
    char all[512] = "";
    for (unsigned long i = 0; i < s->nparams && i < 8; i++) {
        snprintf(s->params[i], 256, "%.*s", (int)*b[i].length, (const char *)b[i].buffer);
        if (i) strncat(all, "|", sizeof all - strlen(all) - 1);
        strncat(all, s->params[i], sizeof all - strlen(all) - 1);
    }
    logf_("bind", all); return 0; }
int mysql_stmt_execute(MYSQL_STMT *s) { (void)s; logf_("execute", ""); return 0; }
int mysql_stmt_fetch(MYSQL_STMT *s) {
    if (s->fetched++) return MYSQL_NO_DATA;
    const char *v = s->nparams ? s->params[0] : "1";
    size_t n = strlen(v); memcpy(s->res[0].buffer, v, n + 1);
    *s->res[0].length = n; *s->res[0].is_null = 0; return 0; }
unsigned long mysql_stmt_param_count(MYSQL_STMT *s) { return s->nparams; }
my_ulonglong mysql_stmt_affected_rows(MYSQL_STMT *s) { (void)s; return 7; }
C
cat > "${WORK}/tmy.c" <<C
#include <string.h>
#include "db.h"
${CHECK_H}
extern char g_log[64][512]; extern int g_nlog;
static int logged(const char *prefix, const char *needle) {
    for (int i = 0; i < g_nlog; i++)
        if (!strncmp(g_log[i], prefix, strlen(prefix)) && (!needle || strstr(g_log[i], needle))) return i + 1;
    return 0;
}
static void reset(void) { g_nlog = 0; }
int main(void) {
    const DbBackend *b = &db_mysql_backend;
    const char *PAY = "x'); DROP TABLE t;--";

    reset();
    int rc = b->open("mysql://u:secret@h:3306/db");
    int opt = logged("option", "42=3"), con = logged("connect", NULL);
    CHECK(rc == 0 && opt && con && opt < con, "DAT-06 mysql: SSL_MODE_REQUIRED is set before connecting (option at %d, connect at %d)", opt, con);

    reset();
    const char *p1[1] = { PAY }; StrArray one = { p1, 1 };
    RowResult r = b->one("SELECT ?", one);
    CHECK(!r.is_err && r.ok.col_count == 1 && !strcmp(r.ok.col_values[0], PAY),
          "DAT-03 mysql one: the parameter is bound and comes back as the value (%s)",
          r.is_err ? r.err.msg : r.ok.col_values[0]);
    CHECK(logged("prepare", "SELECT ?") && logged("bind", PAY) && !logged("query", NULL),
          "DAT-03 mysql one: prepared + bound, nothing sent through mysql_query");

    reset();
    RowArrayResult m = b->many("SELECT ?", one);
    CHECK(!m.is_err && m.ok.len == 1 && !strcmp(m.ok.data[0].col_values[0], PAY) && !logged("query", NULL),
          "DAT-03 mysql many: one bound row, no mysql_query");

    reset();
    const char *p2[1] = { "v" }; StrArray upd = { p2, 1 };
    U64Result e = b->exec("UPDATE t SET a=?", upd);
    CHECK(!e.is_err && e.ok == 7 && logged("bind", "v") && logged("execute", NULL) && !logged("query", NULL),
          "DAT-03 mysql exec: bound, executed, affected rows from the statement (%llu)", (unsigned long long)e.ok);

    reset();
    const char *p3[1] = { "a" }; StrArray short1 = { p3, 1 };
    RowResult mm = b->one("SELECT ?, ?", short1);
    CHECK(mm.is_err && !logged("execute", NULL),
          "mysql: one parameter for two placeholders is refused before executing");

    reset();
    int ex = b->table_exists(0, "x' OR '1'='1");
    CHECK(ex == 1 && logged("prepare", "table_name=?") && logged("bind", "x' OR '1'='1") && !logged("query", "OR '1'"),
          "DAT-04 mysql table_exists: the name is bound, not formatted into the SQL");

    reset();
    StrArray none = { NULL, 0 };
    (void)b->exec("CREATE TABLE t(a INT); CREATE TABLE u(b INT)", none);
    CHECK(logged("query", "CREATE TABLE t") && !logged("prepare", NULL),
          "control: a call with no parameters still goes through mysql_query");

    printf("HARNESS %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
C
if "${CC}" -std=c11 -D_GNU_SOURCE ${SAN} -DTK_HAVE_MYSQL -I"${WORK}/mock" -I"${S}" \
        -o "${WORK}/tmy" "${WORK}/tmy.c" "${WORK}/mock.c" "${S}/db_mysql.c" >"${WORK}/my.cc" 2>&1; then
    "${WORK}/tmy" >"${WORK}/tmy.out" 2>&1; collect "${WORK}/tmy.out" mysql
else
    fail "mysql harness did not build"; head -15 "${WORK}/my.cc" | sed 's/^/      | /'
fi

# ── Postgres, against the real libpq ───────────────────────────────────────
PQ=""
for pc in "" /opt/homebrew/opt/libpq/lib/pkgconfig /usr/local/opt/libpq/lib/pkgconfig; do
    if PKG_CONFIG_PATH="${pc}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" pkg-config --exists libpq 2>/dev/null; then
        PQ="$(PKG_CONFIG_PATH="${pc}${PKG_CONFIG_PATH:+:${PKG_CONFIG_PATH}}" pkg-config --cflags --libs libpq)"; break
    fi
done
if [ -z "${PQ}" ]; then
    echo "  SKIP: DAT-06 postgres -- libpq not found by pkg-config (the MySQL and SQLite cases still ran)"
    SKIP=$((SKIP + 1))
else
    cat > "${WORK}/pqhook.h" <<'H'
#include <libpq-fe.h>
PGconn *c512_connectdb(const char *dsn);
PGconn *c512_connectdbparams(const char *const *k, const char *const *v, int expand);
#define PQconnectdb(d) c512_connectdb(d)
#define PQconnectdbParams(k, v, e) c512_connectdbparams(k, v, e)
H
    cat > "${WORK}/tpg.c" <<C
#include <string.h>
#include <libpq-fe.h>
#include "db.h"
${CHECK_H}
static char g_sslmode[64];
static PGconn *note(PGconn *c) {
    g_sslmode[0] = 0;
    PQconninfoOption *o = PQconninfo(c);
    for (PQconninfoOption *i = o; i && i->keyword; i++)
        if (!strcmp(i->keyword, "sslmode") && i->val) snprintf(g_sslmode, sizeof g_sslmode, "%s", i->val);
    PQconninfoFree(o);
    return c;
}
PGconn *c512_connectdb(const char *dsn) { return note(PQconnectdb(dsn)); }
PGconn *c512_connectdbparams(const char *const *k, const char *const *v, int e) {
    return note(PQconnectdbParams(k, v, e)); }
int main(void) {
    const DbBackend *b = &db_postgres_backend;
    /* port 1 on loopback refuses at once; only the options are of interest */
    (void)b->open("host=127.0.0.1 port=1 dbname=x connect_timeout=2");
    CHECK(!strcmp(g_sslmode, "require"), "DAT-06 postgres: a DSN without sslmode connects with sslmode=%s (want require)", g_sslmode);
    (void)b->open("host=127.0.0.1 port=1 dbname=x sslmode=disable connect_timeout=2");
    CHECK(!strcmp(g_sslmode, "disable"), "control: a DSN that names sslmode=disable still decides (%s)", g_sslmode);
    (void)b->open("postgresql://127.0.0.1:1/x?sslmode=verify-full&connect_timeout=2");
    CHECK(!strcmp(g_sslmode, "verify-full"), "control: a URI DSN's sslmode=verify-full still decides (%s)", g_sslmode);
    printf("HARNESS %s\n", fails ? "FAILED" : "OK");
    return fails != 0;
}
C
    # shellcheck disable=SC2086
    if "${CC}" -std=c11 -D_GNU_SOURCE ${SAN} -DTK_HAVE_LIBPQ -I"${S}" ${PQ} -c -o "${WORK}/pg.o" \
            -include "${WORK}/pqhook.h" "${S}/db_postgres.c" >"${WORK}/pg.cc" 2>&1 \
       && "${CC}" -std=c11 -D_GNU_SOURCE ${SAN} -DTK_HAVE_LIBPQ -I"${S}" -o "${WORK}/tpg" \
            "${WORK}/tpg.c" "${WORK}/pg.o" ${PQ} >>"${WORK}/pg.cc" 2>&1; then
        "${WORK}/tpg" >"${WORK}/tpg.out" 2>&1; collect "${WORK}/tpg.out" postgres
    else
        fail "postgres harness did not build"; head -15 "${WORK}/pg.cc" | sed 's/^/      | /'
    fi
fi

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
if "${CC}" -std=c11 -D_GNU_SOURCE ${SAN} -I"${S}" -c -o "${WORK}/db.o" -include "${WORK}/sqhook.h" \
        "${S}/db.c" >"${WORK}/sq.cc" 2>&1 \
   && "${CC}" -std=c11 -D_GNU_SOURCE ${SAN} -I"${S}" -o "${WORK}/tsq" "${WORK}/tsq.c" "${WORK}/db.o" -lsqlite3 \
        >>"${WORK}/sq.cc" 2>&1; then
    "${WORK}/tsq" >"${WORK}/tsq.out" 2>&1; collect "${WORK}/tsq.out" sqlite
else
    fail "sqlite harness did not build"; head -15 "${WORK}/sq.cc" | sed 's/^/      | /'
fi

echo "-------------------------------------------------------------------------------"
echo "Results: ${PASS} passed, ${FAIL} failed, ${SKIP} skipped"
[ "${FAIL}" -eq 0 ]
