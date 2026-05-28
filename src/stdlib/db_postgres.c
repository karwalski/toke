/* db_postgres.c — PostgreSQL backend for std.db (Story 57.10.2)
 * Compile with: -DTK_HAVE_LIBPQ $(pkg-config --cflags --libs libpq) */

#ifdef TK_HAVE_LIBPQ

#include "db.h"
#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static PGconn *g_pg = NULL;

static DbErr pg_err(DbErrKind kind, const char *msg)
{
    DbErr e; e.kind = kind; e.msg = msg; return e;
}

/* Rewrite ? placeholders to $1, $2, ... for libpq. */
static char *rewrite_placeholders(const char *sql)
{
    /* Count ? marks */
    int count = 0;
    for (const char *p = sql; *p; p++)
        if (*p == '?') count++;
    if (count == 0) return strdup(sql);

    /* Each $N can be up to 6 chars ($99999) — allocate generously */
    size_t len = strlen(sql) + (size_t)count * 6 + 1;
    char *out = malloc(len);
    char *wp = out;
    int idx = 0;
    for (const char *rp = sql; *rp; rp++) {
        if (*rp == '?') {
            idx++;
            wp += sprintf(wp, "$%d", idx);
        } else {
            *wp++ = *rp;
        }
    }
    *wp = '\0';
    return out;
}

static int pg_open(const char *dsn)
{
    g_pg = PQconnectdb(dsn);
    if (PQstatus(g_pg) != CONNECTION_OK) {
        PQfinish(g_pg); g_pg = NULL; return -1;
    }
    return 0;
}

static void pg_close(void)
{
    if (g_pg) { PQfinish(g_pg); g_pg = NULL; }
}

static Row pg_collect_row(PGresult *res, int row_idx)
{
    int n = PQnfields(res);
    const char **names  = malloc((size_t)n * sizeof(char *));
    const char **values = malloc((size_t)n * sizeof(char *));
    int         *nulls  = malloc((size_t)n * sizeof(int));
    for (int c = 0; c < n; c++) {
        names[c]  = strdup(PQfname(res, c));
        nulls[c]  = PQgetisnull(res, row_idx, c);
        const char *v = PQgetvalue(res, row_idx, c);
        values[c] = v ? strdup(v) : strdup("");
    }
    Row row;
    row.col_names  = names;
    row.col_values = values;
    row.col_nulls  = nulls;
    row.col_count  = (uint64_t)n;
    return row;
}

static RowResult pg_one(const char *sql, StrArray params)
{
    RowResult r; r.is_err = 0;
    char *rewritten = rewrite_placeholders(sql);
    PGresult *res = PQexecParams(g_pg, rewritten, (int)params.len,
                                 NULL, params.data, NULL, NULL, 0);
    free(rewritten);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
        PQclear(res);
        return r;
    }
    if (PQntuples(res) == 0) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_NOT_FOUND, "no rows returned");
        PQclear(res);
        return r;
    }
    r.ok = pg_collect_row(res, 0);
    PQclear(res);
    return r;
}

static RowArrayResult pg_many(const char *sql, StrArray params)
{
    RowArrayResult r; r.is_err = 0;
    char *rewritten = rewrite_placeholders(sql);
    PGresult *res = PQexecParams(g_pg, rewritten, (int)params.len,
                                 NULL, params.data, NULL, NULL, 0);
    free(rewritten);
    if (PQresultStatus(res) != PGRES_TUPLES_OK) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
        PQclear(res);
        return r;
    }
    int nrows = PQntuples(res);
    Row *rows = malloc((size_t)nrows * sizeof(Row));
    for (int i = 0; i < nrows; i++)
        rows[i] = pg_collect_row(res, i);
    r.ok.data = rows;
    r.ok.len  = (uint64_t)nrows;
    PQclear(res);
    return r;
}

static U64Result pg_exec(const char *sql, StrArray params)
{
    U64Result r; r.is_err = 0; r.ok = 0;
    char *rewritten = rewrite_placeholders(sql);
    PGresult *res = PQexecParams(g_pg, rewritten, (int)params.len,
                                 NULL, params.data, NULL, NULL, 0);
    free(rewritten);
    ExecStatusType status = PQresultStatus(res);
    if (status != PGRES_COMMAND_OK && status != PGRES_TUPLES_OK) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
        PQclear(res);
        return r;
    }
    const char *affected = PQcmdTuples(res);
    if (affected && *affected)
        r.ok = (uint64_t)strtoull(affected, NULL, 10);
    PQclear(res);
    return r;
}

static BoolResult pg_begin(int conn_id)
{
    (void)conn_id;
    BoolResult r; r.is_err = 0; r.ok = 1;
    PGresult *res = PQexec(g_pg, "BEGIN");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        r.is_err = 1; r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
    }
    PQclear(res);
    return r;
}

static BoolResult pg_commit(int conn_id)
{
    (void)conn_id;
    BoolResult r; r.is_err = 0; r.ok = 1;
    PGresult *res = PQexec(g_pg, "COMMIT");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        r.is_err = 1; r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
    }
    PQclear(res);
    return r;
}

static BoolResult pg_rollback(int conn_id)
{
    (void)conn_id;
    BoolResult r; r.is_err = 0; r.ok = 1;
    PGresult *res = PQexec(g_pg, "ROLLBACK");
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        r.is_err = 1; r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
    }
    PQclear(res);
    return r;
}

static U64Result pg_last_insert_id(int conn_id)
{
    (void)conn_id;
    /* PostgreSQL uses RETURNING clause instead of last_insert_id.
     * This queries the last value from any serial/identity column. */
    U64Result r; r.is_err = 0; r.ok = 0;
    PGresult *res = PQexec(g_pg, "SELECT lastval()");
    if (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0) {
        r.ok = (uint64_t)strtoull(PQgetvalue(res, 0, 0), NULL, 10);
    } else {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, "no sequence value available");
    }
    PQclear(res);
    return r;
}

static U64Result pg_affected_rows(int conn_id)
{
    (void)conn_id;
    /* Caller should use return value from db_exec instead */
    U64Result r; r.is_err = 1; r.ok = 0;
    r.err = pg_err(DB_ERR_QUERY, "use db_exec return value");
    return r;
}

static int pg_table_exists(int conn_id, const char *name)
{
    (void)conn_id;
    if (!g_pg) return 0;
    const char *sql =
        "SELECT 1 FROM information_schema.tables "
        "WHERE table_schema='public' AND table_name=$1";
    const char *vals[1] = { name };
    PGresult *res = PQexecParams(g_pg, sql, 1, NULL, vals, NULL, NULL, 0);
    int exists = (PQresultStatus(res) == PGRES_TUPLES_OK && PQntuples(res) > 0);
    PQclear(res);
    return exists;
}

/* Internal struct for PostgreSQL prepared statements, cast to/from TkStmt*. */
static unsigned int pg_stmt_counter = 0;

typedef struct {
    char      name[32];    /* PQprepare statement name ("_tk_s0", "_tk_s1", ...) */
    char     *sql;         /* rewritten SQL with $N placeholders                 */
    PGresult *result;      /* result set from last PQexecPrepared                */
    int       row_idx;     /* current row cursor within result                   */
    int       total_rows;  /* total rows in result set                           */
} PgStmt;

static StmtResult pg_prepare(int conn_id, const char *sql)
{
    (void)conn_id;
    StmtResult r; r.is_err = 0; r.ok = NULL;

    if (!g_pg) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_CONNECTION, "no connection");
        return r;
    }

    PgStmt *s = calloc(1, sizeof(PgStmt));
    snprintf(s->name, sizeof(s->name), "_tk_s%u", pg_stmt_counter++);
    s->sql = rewrite_placeholders(sql);

    PGresult *res = PQprepare(g_pg, s->name, s->sql, 0, NULL);
    if (PQresultStatus(res) != PGRES_COMMAND_OK) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
        PQclear(res);
        free(s->sql);
        free(s);
        return r;
    }
    PQclear(res);

    r.ok = (TkStmt *)s;
    return r;
}

static BoolResult pg_bind(TkStmt *stmt, StrArray params)
{
    BoolResult r; r.is_err = 0; r.ok = 1;
    if (!stmt) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, "null statement");
        return r;
    }
    PgStmt *s = (PgStmt *)stmt;

    /* Clear any previous result set. */
    if (s->result) {
        PQclear(s->result);
        s->result = NULL;
    }

    s->result = PQexecPrepared(g_pg, s->name, (int)params.len,
                               params.data, NULL, NULL, 0);

    ExecStatusType status = PQresultStatus(s->result);
    if (status != PGRES_TUPLES_OK && status != PGRES_COMMAND_OK) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, PQerrorMessage(g_pg));
        PQclear(s->result);
        s->result = NULL;
        return r;
    }

    s->row_idx    = 0;
    s->total_rows = (status == PGRES_TUPLES_OK) ? PQntuples(s->result) : 0;
    return r;
}

static RowResult pg_step(TkStmt *stmt)
{
    RowResult r; r.is_err = 0;
    if (!stmt) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_QUERY, "null statement");
        return r;
    }
    PgStmt *s = (PgStmt *)stmt;

    if (!s->result || s->row_idx >= s->total_rows) {
        r.is_err = 1;
        r.err = pg_err(DB_ERR_NOT_FOUND, "done");
        return r;
    }

    r.ok = pg_collect_row(s->result, s->row_idx);
    s->row_idx++;
    return r;
}

static void pg_finalize(TkStmt *stmt)
{
    if (!stmt) return;
    PgStmt *s = (PgStmt *)stmt;

    if (s->result) PQclear(s->result);

    /* Deallocate the server-side prepared statement. */
    if (g_pg) {
        char dealloc[64];
        snprintf(dealloc, sizeof(dealloc), "DEALLOCATE %s", s->name);
        PGresult *res = PQexec(g_pg, dealloc);
        if (res) PQclear(res);
    }

    free(s->sql);
    free(s);
}

const DbBackend db_postgres_backend = {
    .name            = "postgres",
    .open            = pg_open,
    .close           = pg_close,
    .one             = pg_one,
    .many            = pg_many,
    .exec            = pg_exec,
    .prepare         = pg_prepare,
    .bind            = pg_bind,
    .step            = pg_step,
    .finalize        = pg_finalize,
    .begin           = pg_begin,
    .commit          = pg_commit,
    .rollback        = pg_rollback,
    .last_insert_id  = pg_last_insert_id,
    .affected_rows   = pg_affected_rows,
    .table_exists    = pg_table_exists,
};

#endif /* TK_HAVE_LIBPQ */
