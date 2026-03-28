/* db.c — std.db implementation (SQLite3 backend). Link: -lsqlite3
 * Story: 1.3.3  Branch: feature/stdlib-db */

#include "db.h"
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>

static sqlite3 *g_db = NULL;

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

/* Collect current stmt row into a heap-allocated Row. */
static Row collect_row(sqlite3_stmt *stmt)
{
    int n = sqlite3_column_count(stmt);
    const char **names  = malloc((size_t)n * sizeof(char *));
    const char **values = malloc((size_t)n * sizeof(char *));
    for (int c = 0; c < n; c++) {
        names[c]  = sqlite3_column_name(stmt, c);
        const char *v = (const char *)sqlite3_column_text(stmt, c);
        values[c] = v ? v : "";
    }
    Row row; row.col_names = names; row.col_values = values;
    row.col_count = (uint64_t)n;
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
