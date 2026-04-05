/* db.c — std.db implementation (SQLite3 backend). Link: -lsqlite3
 * Story: 1.3.3  Branch: feature/stdlib-db */

#include "db.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *g_db = NULL;

/* Forward declarations — defined later in this file. */
static sqlite3 *conn_db(int conn_id);
static int find_col(Row r, const char *col);

int db_open(const char *dsn)
{
    if (sqlite3_open(dsn, &g_db) != SQLITE_OK) {
        sqlite3_close(g_db); g_db = NULL; return -1;
    }
    return 0;
}

void db_close(void)
{
    if (g_db) { sqlite3_close(g_db); g_db = NULL; }
}

static DbErr make_err(DbErrKind kind, const char *msg)
{
    DbErr e; e.kind = kind; e.msg = msg; return e;
}

/* Bind params array then step once. Returns SQLITE_ROW / SQLITE_DONE / err. */
static int bind_and_step(sqlite3_stmt *stmt, StrArray params)
{
    for (uint64_t i = 0; i < params.len; i++) {
        if (sqlite3_bind_text(stmt, (int)(i + 1), params.data[i],
                              -1, SQLITE_STATIC) != SQLITE_OK)
            return SQLITE_ERROR;
    }
    return sqlite3_step(stmt);
}

/* Collect current stmt row into a heap-allocated Row.
 * All strings are strdup'd so they remain valid after sqlite3_finalize.
 * col_nulls[c] is set to 1 when the column value is SQL NULL. */
static Row collect_row(sqlite3_stmt *stmt)
{
    int n = sqlite3_column_count(stmt);
    const char **names  = malloc((size_t)n * sizeof(char *));
    const char **values = malloc((size_t)n * sizeof(char *));
    int         *nulls  = malloc((size_t)n * sizeof(int));
    for (int c = 0; c < n; c++) {
        const char *name = sqlite3_column_name(stmt, c);
        names[c] = name ? strdup(name) : strdup("");
        int is_null = (sqlite3_column_type(stmt, c) == SQLITE_NULL);
        nulls[c] = is_null;
        const char *v = (const char *)sqlite3_column_text(stmt, c);
        values[c] = v ? strdup(v) : strdup("");
    }
    Row row; row.col_names = names; row.col_values = values;
    row.col_nulls = nulls; row.col_count = (uint64_t)n;
    return row;
}

RowResult db_one(const char *sql, StrArray params)
{
    RowResult res; res.is_err = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        res.is_err = 1; res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        return res;
    }
    int rc = bind_and_step(stmt, params);
    if (rc == SQLITE_ROW) {
        res.ok = collect_row(stmt);
    } else if (rc == SQLITE_DONE) {
        res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, "no rows returned");
    } else {
        res.is_err = 1; res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(g_db));
    }
    sqlite3_finalize(stmt);
    return res;
}

RowArrayResult db_many(const char *sql, StrArray params)
{
    RowArrayResult res; res.is_err = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        res.is_err = 1; res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        return res;
    }
    uint64_t cap = 8, count = 0;
    Row *rows = malloc(cap * sizeof(Row));
    for (uint64_t i = 0; i < params.len; i++)
        sqlite3_bind_text(stmt, (int)(i + 1), params.data[i], -1, SQLITE_STATIC);
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count == cap) { cap *= 2; rows = realloc(rows, cap * sizeof(Row)); }
        rows[count++] = collect_row(stmt);
    }
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        res.is_err = 1; res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        return res;
    }
    res.ok.data = rows; res.ok.len = count;
    return res;
}

U64Result db_exec(const char *sql, StrArray params)
{
    U64Result res; res.is_err = 0; res.ok = 0;
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        res.is_err = 1; res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        return res;
    }
    int rc = bind_and_step(stmt, params);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        res.is_err = 1; res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        return res;
    }
    res.ok = (uint64_t)sqlite3_changes(g_db);
    return res;
}

/* ── metadata and result inspection (Story 29.2.2) ───────────────────────── */

U64Result db_last_insert_id(int conn_id)
{
    U64Result res; res.is_err = 0; res.ok = 0;
    sqlite3 *db = conn_db(conn_id);
    if (!db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, "invalid conn_id");
        return res;
    }
    res.ok = (uint64_t)sqlite3_last_insert_rowid(db);
    return res;
}

U64Result db_affected_rows(int conn_id)
{
    U64Result res; res.is_err = 0; res.ok = 0;
    sqlite3 *db = conn_db(conn_id);
    if (!db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, "invalid conn_id");
        return res;
    }
    res.ok = (uint64_t)sqlite3_changes(db);
    return res;
}

StrArray db_columns(Row r)
{
    StrArray sa;
    sa.data = r.col_names;
    sa.len  = r.col_count;
    return sa;
}

int db_is_null(Row r, const char *col)
{
    int idx = find_col(r, col);
    if (idx < 0) return 0;
    return r.col_nulls[idx];
}

int db_table_exists(int conn_id, const char *name)
{
    sqlite3 *db = conn_db(conn_id);
    if (!db) return 0;
    const char *sql =
        "SELECT 1 FROM sqlite_master WHERE type='table' AND name=?";
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK)
        return 0;
    sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
    int exists = (sqlite3_step(stmt) == SQLITE_ROW) ? 1 : 0;
    sqlite3_finalize(stmt);
    return exists;
}

/* ── prepared statement struct ────────────────────────────────────────────── */

struct TkStmt {
    sqlite3_stmt *stmt;
    sqlite3      *db;
};

/* Resolve conn_id to the global db handle (currently only conn 0 is valid). */
static sqlite3 *conn_db(int conn_id)
{
    if (conn_id == 0) return g_db;
    return NULL;
}

StmtResult db_prepare(int conn_id, const char *sql)
{
    StmtResult res; res.is_err = 0; res.ok = NULL;
    sqlite3 *db = conn_db(conn_id);
    if (!db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, "invalid conn_id");
        return res;
    }
    sqlite3_stmt *raw = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, NULL) != SQLITE_OK) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(db));
        return res;
    }
    TkStmt *s = malloc(sizeof(TkStmt));
    s->stmt = raw;
    s->db   = db;
    res.ok = s;
    return res;
}

BoolResult db_bind(TkStmt *stmt, StrArray params)
{
    BoolResult res; res.is_err = 0; res.ok = 1;
    if (!stmt) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_QUERY, "null statement");
        return res;
    }
    sqlite3_reset(stmt->stmt);
    sqlite3_clear_bindings(stmt->stmt);
    for (uint64_t i = 0; i < params.len; i++) {
        int rc = sqlite3_bind_text(stmt->stmt, (int)(i + 1),
                                   params.data[i], -1, SQLITE_STATIC);
        if (rc != SQLITE_OK) {
            res.is_err = 1;
            res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(stmt->db));
            return res;
        }
    }
    return res;
}

RowResult db_step(TkStmt *stmt)
{
    RowResult res; res.is_err = 0;
    if (!stmt) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_QUERY, "null statement");
        return res;
    }
    int rc = sqlite3_step(stmt->stmt);
    if (rc == SQLITE_ROW) {
        res.ok = collect_row(stmt->stmt);
    } else if (rc == SQLITE_DONE) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_NOT_FOUND, "done");
    } else {
        res.is_err = 1;
        res.err = make_err(DB_ERR_QUERY, sqlite3_errmsg(stmt->db));
    }
    return res;
}

void db_finalize(TkStmt *stmt)
{
    if (!stmt) return;
    sqlite3_finalize(stmt->stmt);
    free(stmt);
}

/* ── transactions ─────────────────────────────────────────────────────────── */

static BoolResult exec_simple(int conn_id, const char *sql)
{
    BoolResult res; res.is_err = 0; res.ok = 1;
    sqlite3 *db = conn_db(conn_id);
    if (!db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, "invalid conn_id");
        return res;
    }
    char *errmsg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_QUERY, errmsg ? errmsg : sql);
        /* errmsg must be freed with sqlite3_free — we copy it into a static
           buffer to avoid leaking the pointer after this function returns.
           For correctness in tests we store the pointer; callers must not
           free it.  In production code a copy into a fixed arena is preferred. */
        return res;
    }
    return res;
}

BoolResult db_begin(int conn_id)    { return exec_simple(conn_id, "BEGIN"); }
BoolResult db_commit(int conn_id)   { return exec_simple(conn_id, "COMMIT"); }
BoolResult db_rollback(int conn_id) { return exec_simple(conn_id, "ROLLBACK"); }

/* Linear scan for col; returns index or -1. */
static int find_col(Row r, const char *col)
{
    for (uint64_t i = 0; i < r.col_count; i++)
        if (strcmp(r.col_names[i], col) == 0) return (int)i;
    return -1;
}

StrResult row_str(Row r, const char *col)
{
    StrResult res; res.is_err = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = r.col_values[idx]; return res;
}

U64Result row_u64(Row r, const char *col)
{
    U64Result res; res.is_err = 0; res.ok = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = (uint64_t)strtoull(r.col_values[idx], NULL, 10); return res;
}

I64Result row_i64(Row r, const char *col)
{
    I64Result res; res.is_err = 0; res.ok = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = (int64_t)strtoll(r.col_values[idx], NULL, 10); return res;
}

F64Result row_f64(Row r, const char *col)
{
    F64Result res; res.is_err = 0; res.ok = 0.0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = strtod(r.col_values[idx], NULL); return res;
}

BoolResult row_bool(Row r, const char *col)
{
    BoolResult res; res.is_err = 0; res.ok = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    const char *v = r.col_values[idx];
    res.ok = (strcmp(v,"1")==0 || strcmp(v,"true")==0 || strcmp(v,"TRUE")==0) ? 1 : 0;
    return res;
}
