#!/usr/bin/env bash
# C523_postgres_placeholders_outside_literals.sh — the PostgreSQL backend
# turns '?' into $N only where '?' is a placeholder (story 121.49 item 3).
#
# THE DEFECT, measured on main (7014e929):
#   db_postgres.c rewrite_placeholders rewrote every '?' byte, so
#   `SELECT '?', ?` reached libpq as `SELECT '$1', $2`: the literal changed
#   and the statement then wanted two parameters where the caller passed
#   one.  The same inside "quoted identifiers", $$dollar quotes$$, and
#   -- / /* */ comments.  It also sized its buffer at 6 bytes per '?' and
#   wrote with sprintf, unchecked malloc.
#
# NOW: a '?' inside '...', E'...' (backslash escapes), "...", $tag$...$tag$,
# a -- comment or a (nested) /* */ comment is left alone; every other '?'
# is numbered in order.  Handled and not handled are listed in
# db_postgres.c (the jsonb ?, ?|, ?& operators are still rewritten).
#
# WHAT THIS DRIVES.  libpq is not needed: the harness supplies a stub
# <libpq-fe.h> whose PQexecParams/PQprepare record the SQL they are given,
# #includes src/stdlib/db_postgres.c with -DTK_HAVE_LIBPQ, and checks the
# rewrite directly (17 cases) and through the backend's exec, one, many and
# prepare entry points.  Built with ASan+UBSan.
#
# Negative control: C523_PG_C=<file> builds against another db_postgres.c;
# main's fails every case with a '?' inside a literal, identifier, dollar
# quote or comment.
#
# Story: 121.49

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
PG_C="${C523_PG_C:-${S}/db_postgres.c}"

WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c523_XXXXXX")"
trap 'rm -rf "${WORK}"' EXIT

echo "C523: postgres ? placeholders are not rewritten inside literals or comments"
echo "--------------------------------------------------------------------------"

mkdir -p "${WORK}/inc"
cat > "${WORK}/inc/libpq-fe.h" <<'EOF'
/* Stub libpq for C523: records the SQL handed to it; no server. */
#ifndef C523_LIBPQ_STUB
#define C523_LIBPQ_STUB
typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;
typedef unsigned int Oid;
typedef enum { CONNECTION_OK, CONNECTION_BAD } ConnStatusType;
typedef enum { PGRES_EMPTY_QUERY, PGRES_COMMAND_OK, PGRES_TUPLES_OK,
               PGRES_FATAL_ERROR } ExecStatusType;
extern char c523_last_sql[4096];
extern int  c523_last_nparams;
static PGresult *c523_res(const char *sql, int n) {
    snprintf(c523_last_sql, sizeof c523_last_sql, "%s", sql ? sql : "<NULL>");
    c523_last_nparams = n;
    return (PGresult *)0;
}
static inline PGconn *PQconnectdb(const char *d) { (void)d; return (PGconn *)c523_last_sql; }
static inline ConnStatusType PQstatus(const PGconn *c) { (void)c; return CONNECTION_OK; }
static inline void PQfinish(PGconn *c) { (void)c; }
static inline void PQclear(PGresult *r) { (void)r; }
static inline ExecStatusType PQresultStatus(const PGresult *r) { (void)r; return PGRES_FATAL_ERROR; }
static inline char *PQerrorMessage(const PGconn *c) { (void)c; return (char *)"stub"; }
static inline PGresult *PQexec(PGconn *c, const char *q) { (void)c; return c523_res(q, 0); }
static inline PGresult *PQexecParams(PGconn *c, const char *q, int n, const Oid *t,
        const char *const *v, const int *l, const int *f, int rf)
    { (void)c; (void)t; (void)v; (void)l; (void)f; (void)rf; return c523_res(q, n); }
static inline PGresult *PQprepare(PGconn *c, const char *name, const char *q, int n,
        const Oid *t) { (void)c; (void)name; (void)t; return c523_res(q, n); }
static inline PGresult *PQexecPrepared(PGconn *c, const char *name, int n,
        const char *const *v, const int *l, const int *f, int rf)
    { (void)c; (void)name; (void)n; (void)v; (void)l; (void)f; (void)rf; return (PGresult *)0; }
static inline int PQntuples(const PGresult *r) { (void)r; return 0; }
static inline int PQnfields(const PGresult *r) { (void)r; return 0; }
static inline char *PQfname(const PGresult *r, int c) { (void)r; (void)c; return (char *)""; }
static inline int PQgetisnull(const PGresult *r, int a, int b) { (void)r; (void)a; (void)b; return 1; }
static inline char *PQgetvalue(const PGresult *r, int a, int b) { (void)r; (void)a; (void)b; return (char *)""; }
static inline char *PQcmdTuples(PGresult *r) { (void)r; return (char *)"0"; }
#endif
EOF

cat > "${WORK}/t.c" <<'EOF'
#include <stdio.h>
#include "db_postgres.c"

char c523_last_sql[4096];
int  c523_last_nparams;
int  tk_cap_check(uint32_t cap) { (void)cap; return 1; }
void tk_cap_deny(uint32_t cap) { (void)cap; }

static int fails = 0, checks = 0;
static void check(const char *label, const char *got, const char *want) {
    checks++;
    if (got && strcmp(got, want) == 0) printf("  PASS: %s\n", label);
    else {
        printf("  FAIL: %s\n        got  %s\n        want %s\n", label, got ? got : "<NULL>", want);
        fails++;
    }
}

int main(void) {
    struct { const char *label, *in, *want; } c[] = {
        { "plain placeholders",           "SELECT a FROM t WHERE a = ? AND b = ?",
                                          "SELECT a FROM t WHERE a = $1 AND b = $2" },
        { "no placeholder",               "SELECT 1", "SELECT 1" },
        { "'?' string literal",           "SELECT '?', ?", "SELECT '?', $1" },
        { "'' escaped quote in a literal","SELECT 'it''s ?', ?", "SELECT 'it''s ?', $1" },
        { "plain literal: backslash is not an escape",
                                          "SELECT '\\', ?", "SELECT '\\', $1" },
        { "E'' literal: backslash escapes a quote",
                                          "SELECT E'\\'?', ?", "SELECT E'\\'?', $1" },
        { "e'' lower-case",               "SELECT e'\\'?', ?", "SELECT e'\\'?', $1" },
        { "name ending in E is not an E-string",
                                          "SELECT some'\\' , ?", "SELECT some'\\' , $1" },
        { "\"quoted?identifier\"",        "SELECT \"we?rd\" FROM t WHERE a = ?",
                                          "SELECT \"we?rd\" FROM t WHERE a = $1" },
        { "\"\" inside an identifier",    "SELECT \"a\"\"?\" , ?", "SELECT \"a\"\"?\" , $1" },
        { "$$ dollar quote $$",           "SELECT $$ ? it's $$, ?", "SELECT $$ ? it's $$, $1" },
        { "$tag$ dollar quote $tag$",     "SELECT $fn$ a $$ ? $fn$, ?", "SELECT $fn$ a $$ ? $fn$, $1" },
        { "$1 is not a dollar-quote tag", "SELECT ?, $1x", "SELECT $1, $1x" },
        { "-- line comment",              "SELECT ? -- why?\n, ?", "SELECT $1 -- why?\n, $2" },
        { "nested /* */ comment",         "SELECT /* a? /* b? */ c? */ ?", "SELECT /* a? /* b? */ c? */ $1" },
        { "unterminated literal runs to the end",
                                          "SELECT ?, '?", "SELECT $1, '?" },
        { "ten or more placeholders",     "?,?,?,?,?,?,?,?,?,?,?", "$1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11" },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        char *got = rewrite_placeholders(c[i].in);
        check(c[i].label, got, c[i].want);
        free(got);
    }

    /* Through the backend's entry points. */
    const char *sql = "SELECT '?' AS q, x FROM t WHERE x = ?";
    const char *want = "SELECT '?' AS q, x FROM t WHERE x = $1";
    const char *one[] = { "7" };
    StrArray p; p.data = one; p.len = 1;
    if (pg_open("stub") != 0) { printf("  FAIL: stub open\n"); return 1; }
    c523_last_sql[0] = 0; (void)pg_exec(sql, p);  check("db exec sends", c523_last_sql, want);
    c523_last_sql[0] = 0; (void)pg_one(sql, p);   check("db one sends", c523_last_sql, want);
    c523_last_sql[0] = 0; (void)pg_many(sql, p);  check("db many sends", c523_last_sql, want);
    c523_last_sql[0] = 0; (void)pg_prepare(0, sql); check("db prepare sends", c523_last_sql, want);
    pg_close();

    printf("\nResults: %d passed, %d failed\n", checks - fails, fails);
    return fails ? 1 : 0;
}
EOF

cp "${PG_C}" "${WORK}/db_postgres.c"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# shellcheck disable=SC2086
if ! "${CC}" -std=c99 -D_GNU_SOURCE -DTK_HAVE_LIBPQ -g -O1 ${SAN} -Wall -Wno-unused-function \
        -I"${WORK}/inc" -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 1200 "${WORK}/cc.log")"
    exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t"
rc=$?
echo ""
[ "${rc}" -eq 0 ] && echo "C523: PASS" || echo "C523: FAIL (exit ${rc})"
exit "${rc}"
