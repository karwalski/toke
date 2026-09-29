#!/usr/bin/env bash
# C543_postgres_oom_and_jsonb_escape.sh — db_postgres.c survives every
# allocation failure, never hands libpq a NULL query, and `??` reaches the
# server as the jsonb `?` operator (story 121.51 item 8).
#
# THE DEFECT, measured on main (d3a083b8) + fix/r2-rsa:
#   pg_collect_row wrote through three unchecked mallocs; pg_many's row array
#   and pg_prepare's calloc were unchecked; a NULL from rewrite_placeholders
#   (out of memory) went on to PQexecParams / PQprepare as the query.  And
#   jsonb's ?, ?| and ?& operators could not be written at all: every `?` was
#   a placeholder and `??` became `$1$2`.
#
# DECISION (jsonb): `??` is the escape, as in JDBC -- one literal `?` to the
# server, no parameter.  A single `?` is always a placeholder.  `??` never
# produced valid SQL before, so no working query changes meaning.
#
# WHAT THIS DRIVES.  A stub <libpq-fe.h> (no server) whose result has two
# rows of two columns and which records the SQL it is sent; db_postgres.c is
# #included with -DTK_HAVE_LIBPQ under ASan+UBSan, with malloc/calloc/strdup
# routed through a counter that fails the k-th call.  For each entry point
# (one, many, exec, prepare, step) and every k up to its allocation count:
# no crash, no NULL query reaches the stub, and the result is ok or the
# "out of memory" error.  Then the `??` cases.
#
# Negative control: C543_PG_C=<file> checks another db_postgres.c; the
# pre-fix file stops at the first injected failure (ASan SEGV) and fails the
# `??` cases.
#
# Story: 121.51
set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
CC="${CC:-cc}"
S="${REPO_ROOT}/src/stdlib"
PG_C="${C543_PG_C:-${S}/db_postgres.c}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/tkc_c543_XXXXXX")"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/inc"
echo "C543: postgres backend: allocation failures, NULL query, ?? escape (121.51)"
echo "--------------------------------------------------------------------------"

cat > "${WORK}/inc/libpq-fe.h" <<'EOF'
#ifndef C543_LIBPQ_STUB
#define C543_LIBPQ_STUB
typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;
typedef unsigned int Oid;
typedef enum { CONNECTION_OK, CONNECTION_BAD } ConnStatusType;
typedef enum { PGRES_EMPTY_QUERY, PGRES_COMMAND_OK, PGRES_TUPLES_OK,
               PGRES_FATAL_ERROR } ExecStatusType;
extern char c543_sql[4096];
extern int  c543_null_sql, c543_nparams;
static char c543_conn, c543_res, c543_cmd;   /* a row result; a command result */
static PGresult *c543_send(const char *sql, int n) {
    if (!sql) c543_null_sql++;
    snprintf(c543_sql, sizeof c543_sql, "%s", sql ? sql : "<NULL>");
    c543_nparams = n;
    return (PGresult *)&c543_res;
}
static inline PGconn *PQconnectdb(const char *d) { (void)d; return (PGconn *)&c543_conn; }
static inline PGconn *PQconnectdbParams(const char *const *k, const char *const *v, int e)
    { (void)k; (void)v; (void)e; return (PGconn *)&c543_conn; }
static inline ConnStatusType PQstatus(const PGconn *c) { (void)c; return CONNECTION_OK; }
static inline void PQfinish(PGconn *c) { (void)c; }
static inline void PQclear(PGresult *r) { (void)r; }
static inline ExecStatusType PQresultStatus(const PGresult *r)
    { return !r ? PGRES_FATAL_ERROR : r == (const PGresult *)&c543_cmd ? PGRES_COMMAND_OK : PGRES_TUPLES_OK; }
static inline char *PQerrorMessage(const PGconn *c) { (void)c; return (char *)"stub error"; }
static inline PGresult *PQexec(PGconn *c, const char *q) { (void)c; return c543_send(q, 0); }
static inline PGresult *PQexecParams(PGconn *c, const char *q, int n, const Oid *t,
        const char *const *v, const int *l, const int *f, int rf)
    { (void)c; (void)t; (void)v; (void)l; (void)f; (void)rf; return c543_send(q, n); }
static inline PGresult *PQprepare(PGconn *c, const char *name, const char *q, int n,
        const Oid *t) { (void)c; (void)name; (void)t; (void)c543_send(q, n); return (PGresult *)&c543_cmd; }
static inline PGresult *PQexecPrepared(PGconn *c, const char *name, int n,
        const char *const *v, const int *l, const int *f, int rf)
    { (void)c; (void)name; (void)n; (void)v; (void)l; (void)f; (void)rf; return (PGresult *)&c543_res; }
static inline int PQntuples(const PGresult *r) { (void)r; return 2; }
static inline int PQnfields(const PGresult *r) { (void)r; return 2; }
static inline char *PQfname(const PGresult *r, int c) { (void)r; return (char *)(c ? "b" : "a"); }
static inline int PQgetisnull(const PGresult *r, int a, int b) { (void)r; (void)a; (void)b; return 0; }
static inline char *PQgetvalue(const PGresult *r, int a, int b) { (void)r; (void)a; (void)b; return (char *)"v"; }
static inline char *PQcmdTuples(PGresult *r) { (void)r; return (char *)"2"; }
#endif
EOF

cat > "${WORK}/fault.h" <<'EOF'
#include <stdlib.h>
#include <string.h>
extern long c543_calls, c543_fail_at;
static inline void *c543_malloc(size_t n) { return ++c543_calls == c543_fail_at ? NULL : malloc(n); }
static inline void *c543_calloc(size_t a, size_t b) { return ++c543_calls == c543_fail_at ? NULL : calloc(a, b); }
static inline char *c543_strdup(const char *s) { return ++c543_calls == c543_fail_at ? NULL : strdup(s); }
#define malloc c543_malloc
#define calloc c543_calloc
#define strdup c543_strdup
EOF

cat > "${WORK}/t.c" <<'EOF'
#include <stdio.h>
#include "fault.h"
#include "db_postgres.c"
#undef malloc
#undef calloc
#undef strdup
char c543_sql[4096];
int  c543_null_sql, c543_nparams;
long c543_calls, c543_fail_at;
int  tk_cap_check(uint32_t cap) { (void)cap; return 1; }
void tk_cap_deny(uint32_t cap) { (void)cap; }

static int fails = 0, passes = 0;
#define CHECK(c, ...) do { if (c) passes++; else { fails++; printf("  FAIL: " __VA_ARGS__); printf("\n"); } } while (0)

static void t_free_row(Row *row) {   /* the driver's own: the pre-fix file has none */
    for (uint64_t c = 0; c < row->col_count; c++) {
        free((void *)row->col_names[c]); free((void *)row->col_values[c]);
    }
    free((void *)row->col_names); free((void *)row->col_values); free(row->col_nulls);
}
static int is_oom(DbErr e) { return e.msg && strstr(e.msg, "out of memory"); }

/* op: 0 one, 1 many, 2 exec, 3 prepare, 4 step.  Returns 1 if it completed ok. */
static int run(int op, long fail_at, int *oom) {
    static const char *one[] = { "7" };
    StrArray p; p.data = one; p.len = 1;
    const char *sql = "SELECT a, b FROM t WHERE a = ?";
    c543_calls = 0; c543_fail_at = fail_at; *oom = 0;
    int ok = 0;
    if (op == 0) { RowResult r = pg_one(sql, p); ok = !r.is_err; *oom = r.is_err && is_oom(r.err);
                   if (ok) t_free_row(&r.ok); }
    if (op == 1) { RowArrayResult r = pg_many(sql, p); ok = !r.is_err; *oom = r.is_err && is_oom(r.err);
                   if (ok) { for (uint64_t i = 0; i < r.ok.len; i++) t_free_row(&r.ok.data[i]); free(r.ok.data); } }
    if (op == 2) { U64Result r = pg_exec(sql, p); ok = !r.is_err; *oom = r.is_err && is_oom(r.err); }
    if (op == 3) { StmtResult r = pg_prepare(0, sql); ok = !r.is_err; *oom = r.is_err && is_oom(r.err);
                   if (ok) { c543_fail_at = 0; pg_finalize(r.ok); } }
    if (op == 4) {
        c543_fail_at = 0;
        StmtResult s = pg_prepare(0, sql);
        if (s.is_err) return 0;
        (void)pg_bind(s.ok, p);
        c543_calls = 0; c543_fail_at = fail_at;
        RowResult r = pg_step(s.ok); ok = !r.is_err; *oom = r.is_err && is_oom(r.err);
        if (ok) t_free_row(&r.ok);
        c543_fail_at = 0; pg_finalize(s.ok);
    }
    c543_fail_at = 0;
    return ok;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static const char *names[] = { "db one", "db many", "db exec", "db prepare", "db step" };
    if (pg_open("stub") != 0) { printf("  FAIL: stub open\n"); return 1; }
    for (int op = 0; op < 5; op++) {
        int oom;
        c543_null_sql = 0;
        CHECK(run(op, 0, &oom), "%s: succeeds with no injected failure", names[op]);
        long total = c543_calls;
        int ooms = 0, oks = 0;
        for (long k = 1; k <= total; k++) {
            int ok = run(op, k, &oom);
            if (oom) ooms++; else if (ok) oks++;
            CHECK(ok || oom, "%s: allocation %ld of %ld failed: neither ok nor \"out of memory\"", names[op], k, total);
        }
        CHECK(c543_null_sql == 0, "%s: a NULL query reached libpq %d time(s)", names[op], c543_null_sql);
        CHECK(total > 0 && ooms > 0, "%s: no injected failure reported \"out of memory\" (%ld allocations)", names[op], total);
        printf("  PASS-ish: %s: %ld allocations, each failed in turn: %d out-of-memory errors, %d still ok, no crash, no NULL query\n",
               names[op], total, ooms, oks);
    }
    struct { const char *label, *in, *want; int n; } c[] = {
        { "?? is the jsonb ? operator",   "SELECT d ?? 'k' FROM t WHERE a = ?", "SELECT d ? 'k' FROM t WHERE a = $1", 1 },
        { "??| is ?|",                    "SELECT d ??| array['a','b'], ?",     "SELECT d ?| array['a','b'], $1", 1 },
        { "??& is ?&",                    "SELECT ?, d ??& array['a']",         "SELECT $1, d ?& array['a']", 1 },
        { "??? is ? then a placeholder",  "SELECT d ??? FROM t",                "SELECT d ?$1 FROM t", 1 },
        { "single ? is still a placeholder", "SELECT ?, ?",                     "SELECT $1, $2", 2 },
        { "?? inside a literal is untouched", "SELECT '?\?', ?",               "SELECT '?\?', $1", 1 },
    };
    for (size_t i = 0; i < sizeof c / sizeof c[0]; i++) {
        char *got = rewrite_placeholders(c[i].in);
        CHECK(got && strcmp(got, c[i].want) == 0, "%s: got %s want %s", c[i].label, got ? got : "<NULL>", c[i].want);
        free(got);
    }
    pg_close();
    printf("C-driver: %d passed, %d failed\n", passes, fails);
    return fails ? 1 : 0;
}
EOF

cp "${PG_C}" "${WORK}/db_postgres.c"
SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all"
# shellcheck disable=SC2086
if ! "${CC}" -std=c99 -D_GNU_SOURCE -DTK_HAVE_LIBPQ -g -O1 ${SAN} -Wall -Wno-unused-function \
        -I"${WORK}/inc" -iquote "${WORK}" -iquote "${S}" -o "${WORK}/t" "${WORK}/t.c" >"${WORK}/cc.log" 2>&1; then
    echo "  FAIL: ASan build: $(head -c 1200 "${WORK}/cc.log")"
    echo ""; echo "C543: 0 passed, 1 failed"; exit 1
fi
ASAN_OPTIONS="detect_leaks=0:abort_on_error=0" "${WORK}/t" > "${WORK}/t.out" 2>&1
rc=$?
/usr/bin/grep -a '^  FAIL\|^  PASS-ish\|^C-driver' "${WORK}/t.out" | sed 's/PASS-ish/PASS/'
if /usr/bin/grep -a -q 'AddressSanitizer\|runtime error' "${WORK}/t.out"; then
    echo "  FAIL: sanitizer report: $(/usr/bin/grep -a -m1 'AddressSanitizer\|runtime error' "${WORK}/t.out")"
    rc=1
fi
echo ""
[ "${rc}" -eq 0 ] && echo "C543: PASS" || echo "C543: FAIL (exit ${rc})"
exit "${rc}"
