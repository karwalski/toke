/* db_postgres.c — PostgreSQL backend for std.db (Story 57.10.2)
 * Compile with: -DTK_HAVE_LIBPQ $(pkg-config --cflags --libs libpq) */

#ifdef TK_HAVE_LIBPQ

#include "db.h"
#include "capabilities.h"   /* 124.4c2: postgres connect is a net sink */
#include <libpq-fe.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static PGconn *g_pg = NULL;

static DbErr pg_err(DbErrKind kind, const char *msg)
{
    DbErr e; e.kind = kind; e.msg = msg; return e;
}

/* Rewrite ? placeholders to $1, $2, ... for libpq.
 *
 * 121.49 (3): this rewrote every '?' byte, so `SELECT '?', ?` became
 * `SELECT '$1', $2` -- the literal changed and the parameter count no longer
 * matched the caller's.  A '?' is now a placeholder only outside every
 * PostgreSQL lexical form that can contain one:
 *   '...'        string literal, '' as an escaped quote; E'...' (and e'...')
 *                also takes a backslash escape, so E'\'?' is one literal
 *   "..."        quoted identifier, "" as an escaped quote
 *   $tag$...$tag$  dollar-quoted string, tag empty or an identifier ($1 is
 *                a parameter, not a tag: a tag cannot start with a digit)
 *   -- ...       line comment, to end of line
 *   / * ... * /  block comment, nested as PostgreSQL nests them
 * An unterminated form runs to the end of the text, as the server would read
 * it.  Not handled: the jsonb operators ?, ?| and ?& are still rewritten --
 * there is no telling them from a placeholder lexically; use
 * jsonb_exists()/jsonb_exists_any()/jsonb_exists_all() instead.
 * Returns NULL on allocation failure. */
static int pg_ident_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9') || c == '_' || (unsigned char)c >= 0x80;
}

/* Length of a dollar-quote tag "$tag$" starting at p (p[0] == '$'), or 0. */
static size_t pg_dollar_tag(const char *p)
{
    size_t n = 1;
    if (p[n] != '$') {
        char c = p[n];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' ||
              (unsigned char)c >= 0x80))
            return 0;
        while (pg_ident_char(p[n]) && p[n] != '$') n++;
        if (p[n] != '$') return 0;
    }
    return n + 1;
}

/* Index just past the lexical form that starts at sql[i], or i when no
 * quoted/comment form starts there. */
static size_t pg_skip_form(const char *sql, size_t i)
{
    char c = sql[i];
    if (c == '\'') {
        int esc = i > 0 && (sql[i - 1] == 'E' || sql[i - 1] == 'e') &&
                  (i < 2 || !pg_ident_char(sql[i - 2]));
        for (i++; sql[i]; i++) {
            if (esc && sql[i] == '\\' && sql[i + 1]) { i++; continue; }
            if (sql[i] == '\'') {
                if (sql[i + 1] == '\'') { i++; continue; }
                return i + 1;
            }
        }
        return i;
    }
    if (c == '"') {
        for (i++; sql[i]; i++) {
            if (sql[i] == '"') {
                if (sql[i + 1] == '"') { i++; continue; }
                return i + 1;
            }
        }
        return i;
    }
    if (c == '-' && sql[i + 1] == '-') {
        while (sql[i] && sql[i] != '\n') i++;
        return i;
    }
    if (c == '/' && sql[i + 1] == '*') {
        size_t depth = 1;
        for (i += 2; sql[i] && depth; ) {
            if (sql[i] == '/' && sql[i + 1] == '*') { depth++; i += 2; }
            else if (sql[i] == '*' && sql[i + 1] == '/') { depth--; i += 2; }
            else i++;
        }
        return i;
    }
    if (c == '$' && (i == 0 || !pg_ident_char(sql[i - 1]))) {
        size_t t = pg_dollar_tag(sql + i);
        if (!t) return i;
        const char *close = sql + i + t;
        for (;;) {
            const char *d = strchr(close, '$');
            if (!d) return i + t + strlen(sql + i + t);
            if (strncmp(d, sql + i, t) == 0) return (size_t)(d - sql) + t;
            close = d + 1;
        }
    }
    return i;
}

static char *rewrite_placeholders(const char *sql)
{
    /* Pass 1 counts the placeholders and sizes the output exactly. */
    size_t len = strlen(sql), out_len = 0;
    unsigned long idx = 0;
    char num[24];
    for (size_t i = 0; i < len; ) {
        size_t j = pg_skip_form(sql, i);
        if (j != i) { out_len += j - i; i = j; continue; }
        if (sql[i] == '?') out_len += (size_t)snprintf(num, sizeof num, "$%lu", ++idx);
        else out_len++;
        i++;
    }
    char *out = malloc(out_len + 1);
    if (!out) return NULL;
    /* Pass 2 writes it. */
    size_t w = 0;
    idx = 0;
    for (size_t i = 0; i < len; ) {
        size_t j = pg_skip_form(sql, i);
        if (j != i) { memcpy(out + w, sql + i, j - i); w += j - i; i = j; continue; }
        if (sql[i] == '?') {
            int n = snprintf(num, sizeof num, "$%lu", ++idx);
            memcpy(out + w, num, (size_t)n); w += (size_t)n;
        } else out[w++] = sql[i];
        i++;
    }
    out[w] = '\0';
    return out;
}

/* 121.21 (DAT-06): TLS unless the DSN says otherwise.  PQconnectdb(dsn)
 * took libpq's default sslmode, "prefer", which falls back to plaintext --
 * password and queries -- whenever the server does not offer TLS, silently.
 * sslmode=require is now the default; the DSN is expanded AFTER it
 * (expand_dbname), so a DSN that names its own sslmode (verify-full, or
 * disable for a local socket) still decides. */
static int pg_open(const char *dsn)
{
    TK_REQUIRE(TK_CAP_NET);
    static const char *const keys[] = { "sslmode", "dbname", NULL };
    const char *vals[] = { "require", dsn, NULL };
    g_pg = PQconnectdbParams(keys, vals, 1);
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

#else /* !TK_HAVE_LIBPQ */

/* 135.3d: without the backend macro this file would be an EMPTY translation
 * unit, which ISO C forbids (-Wpedantic) and which is indistinguishable from a
 * file whose whole body was #ifdef-ed out by mistake.  Say so in code. */
typedef int tk_db_postgres_unused;

#endif /* TK_HAVE_LIBPQ */
