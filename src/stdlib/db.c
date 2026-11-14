/* SPDX-License-Identifier: MIT */
/* db.c — std.db implementation (SQLite3 backend). Link: -lsqlite3
 * Story: 1.3.3  Branch: feature/stdlib-db */

#include "db.h"
#include "capabilities.h"   /* 124.4c2: sqlite db file is a read-write fs sink */
#include "tk_strhdr.h"     /* 127.295: a DbErr's composed text is a sealed copy */
#include <sqlite3.h>
#include <stdlib.h>
#include <string.h>
#include "tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdio.h>

/* 138.6: a table of connections, each with its own handle.
 *
 * THE DEFECT.  std.db kept one process-global `sqlite3 *`: a second db_open
 * overwrote it (leaking the first), so every later query -- including ones
 * the program meant for the first database -- went to the database opened
 * last, and db.open's returned "handle" selected nothing (loke T-30/R8).
 *
 * Now every open gets a fresh id (1, 2, 3, ... never reused, so a stale
 * handle cannot reach a later database), and conn_id 0 means "the default
 * connection": the lowest-numbered one still open.  So the handle-less calls
 * (db.one / db.many / db.exec / db.close) keep talking to the FIRST database
 * a program opened, and opening a second one repoints nothing.  The *on
 * forms (db.oneon / db.manyon / db.execon / db.closeon) name a handle. */
typedef struct { int64_t id; sqlite3 *db; } DbSlot;
static DbSlot  *g_conns = NULL;
static int      g_nconns = 0, g_capconns = 0;
static int64_t  g_next_id = 1;

/* Forward declarations — defined later in this file. */
static sqlite3 *conn_db(int64_t conn_id);
static int find_col(Row r, const char *col);

int64_t db_connect(const char *dsn)
{
    TK_REQUIRE(TK_CAP_FS_READ);
    TK_REQUIRE(TK_CAP_FS_WRITE);
    if (!dsn) return -1;
    if (g_nconns == g_capconns) {
        int nc = g_capconns ? g_capconns * 2 : 8;
        DbSlot *n = realloc(g_conns, (size_t)nc * sizeof(DbSlot));
        if (!n) return -1;
        g_conns = n; g_capconns = nc;
    }
    sqlite3 *db = NULL;
    if (sqlite3_open(dsn, &db) != SQLITE_OK) {
        sqlite3_close(db); return -1;
    }
    g_conns[g_nconns].id = g_next_id++;
    g_conns[g_nconns].db = db;
    g_nconns++;
    return g_conns[g_nconns - 1].id;
}

int db_open(const char *dsn)
{
    return db_connect(dsn) > 0 ? 0 : -1;
}

int db_close_conn(int64_t conn_id)
{
    if (g_nconns == 0) return 0;
    int at = -1;
    if (conn_id == 0) at = 0;   /* slots stay in id order: slot 0 is the default */
    else for (int i = 0; i < g_nconns; i++) if (g_conns[i].id == conn_id) { at = i; break; }
    if (at < 0) return 0;
    sqlite3_close(g_conns[at].db);
    memmove(&g_conns[at], &g_conns[at + 1], (size_t)(g_nconns - at - 1) * sizeof(DbSlot));
    g_nconns--;
    return 1;
}

void db_close(void)
{
    (void)db_close_conn(0);
}

/* Scratch for messages this file composes; never handed out -- err_copy()
 * copies it (121.51 (5)). */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
static _Thread_local char g_errbuf[512];
#else
static __thread char g_errbuf[512];
#endif

static DbErr make_err(DbErrKind kind, const char *msg)
{
    DbErr e; e.kind = kind; e.msg = msg; return e;
}

/* 121.51 (5): a DbErr owns its text.  e.msg used to be sqlite3_errmsg()'s
 * pointer, which the next call on the connection overwrites and
 * sqlite3_close() frees, or g_errbuf, which the next composed error
 * overwrites -- so a toke program that kept `e` and read e.msg after another
 * db call saw the later error's text, and after db.close read freed memory
 * (ASan: heap-use-after-free).  Every message that is not a literal now goes
 * through here: a heap copy, never freed, like every other str toke holds. */
static DbErr err_copy(DbErrKind kind, const char *msg)
{
    /* 127.295 (S1 step 5): the copy is sealed (tks_strdup), so the `e.msg`
     * a program reads from db_raise's box carries its length; it is still
     * never freed.  A C caller reads it as the C string it always was. */
    char *c = tks_strdup(msg ? msg : "");
    return make_err(kind, c ? c : "db: out of memory copying the error message");
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
/* Release everything collect_row() allocated for one row. */
static void free_row(Row *row)
{
    for (uint64_t c = 0; c < row->col_count; c++) {
        if (row->col_names)  free((void *)row->col_names[c]);
        if (row->col_values) free((void *)row->col_values[c]);
    }
    free((void *)row->col_names);
    free((void *)row->col_values);
    free(row->col_nulls);
}

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

RowResult db_one_on(int64_t conn_id, const char *sql, StrArray params)
{
    RowResult res = {0}; res.is_err = 0;
    sqlite3 *g_db = conn_db(conn_id);
    if (!g_db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, conn_id ? "db: no open connection with that handle"
                                                     : "db: no open connection (call db.open first)");
        return res;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        res.is_err = 1; res.err = err_copy(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        return res;
    }
    int rc = bind_and_step(stmt, params);
    if (rc == SQLITE_ROW) {
        res.ok = collect_row(stmt);
    } else if (rc == SQLITE_DONE) {
        res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, "no rows returned");
    } else {
        res.is_err = 1; res.err = err_copy(DB_ERR_QUERY, sqlite3_errmsg(g_db));
    }
    sqlite3_finalize(stmt);
    return res;
}

RowArrayResult db_many_on(int64_t conn_id, const char *sql, StrArray params)
{
    RowArrayResult res = {0}; res.is_err = 0;
    sqlite3 *g_db = conn_db(conn_id);
    if (!g_db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, conn_id ? "db: no open connection with that handle"
                                                     : "db: no open connection (call db.open first)");
        return res;
    }
    sqlite3_stmt *stmt = NULL;
    if (sqlite3_prepare_v2(g_db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        res.is_err = 1; res.err = err_copy(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        return res;
    }
    uint64_t cap = 8, count = 0;
    Row *rows = malloc(cap * sizeof(Row));
    if (!rows) {
        sqlite3_finalize(stmt);
        res.is_err = 1; res.err = make_err(DB_ERR_QUERY, "out of memory");
        return res;
    }
    /* 121.21: a bind that fails (more params than `?`s: SQLITE_RANGE) is an
     * error, as it is in db_one/db_exec's bind_and_step.  It was ignored, so
     * db.many ran the query with the unbound placeholders as NULL. */
    for (uint64_t i = 0; i < params.len; i++) {
        if (sqlite3_bind_text(stmt, (int)(i + 1), params.data[i], -1,
                              SQLITE_STATIC) != SQLITE_OK) {
            snprintf(g_errbuf, sizeof g_errbuf, "db.many: parameter %llu: %s",
                     (unsigned long long)(i + 1), sqlite3_errmsg(g_db));
            sqlite3_finalize(stmt);
            free(rows);
            res.is_err = 1; res.err = err_copy(DB_ERR_QUERY, g_errbuf);
            return res;
        }
    }
    int rc;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        if (count == cap) {
            /* 135.3c: through a temporary -- realloc's NULL used to overwrite
             * `rows` and the next store wrote through it. */
            Row *nr = realloc(rows, cap * 2 * sizeof(Row));
            if (!nr) { rc = SQLITE_NOMEM; break; }
            rows = nr; cap *= 2;
        }
        rows[count++] = collect_row(stmt);
    }
    if (rc != SQLITE_DONE) {
        /* 135.3c: a query that failed part-way returned the error and leaked
         * every row already collected (clang unix.Malloc). */
        sqlite3_finalize(stmt);   /* before errmsg, as before: it may reset it */
        res.is_err = 1;
        res.err = rc == SQLITE_NOMEM ? make_err(DB_ERR_QUERY, "out of memory")
                                     : err_copy(DB_ERR_QUERY, sqlite3_errmsg(g_db));
        for (uint64_t i = 0; i < count; i++) free_row(&rows[i]);
        free(rows);
        return res;
    }
    sqlite3_finalize(stmt);
    res.ok.data = rows; res.ok.len = count;
    return res;
}

/* 138.6: skip whitespace and SQL comments; returns the first byte of the
 * next statement, or of the terminating NUL when nothing but whitespace,
 * comments and empty statements (";") is left. */
static const char *skip_sql_blank(const char *p)
{
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' ||
               *p == '\v' || *p == ';') p++;
        if (p[0] == '-' && p[1] == '-') {
            while (*p && *p != '\n') p++;
            continue;
        }
        if (p[0] == '/' && p[1] == '*') {
            const char *e = strstr(p + 2, "*/");
            if (!e) return p + strlen(p);
            p = e + 2;
            continue;
        }
        return p;
    }
}

/* 138.6: db.exec runs EVERY statement it is given, or refuses.
 *
 * THE DEFECT.  sqlite3_prepare_v2 compiles only the first statement of its
 * input and hands the rest back through its tail pointer, which db_exec
 * passed as NULL: "CREATE TABLE x(a); CREATE INDEX ix_x ON x(a);" created x,
 * silently dropped the index, and returned ok (loke T-40).
 *
 * Now each statement is prepared from the previous one's tail and run in
 * turn.  A failure stops the run and is returned as $query naming the
 * statement (1-based); the statements before it have been applied -- wrap
 * the string in BEGIN; ... COMMIT; to make it all-or-nothing.  Bind
 * parameters are refused, before anything runs, when the string holds more
 * than one statement: which statement a `?` belongs to is ambiguous.
 * The ok value is the number of rows the whole string inserted, updated or
 * deleted (sqlite3_total_changes before and after). */
U64Result db_exec_on(int64_t conn_id, const char *sql, StrArray params)
{
    U64Result res = {0}; res.is_err = 0; res.ok = 0;
    sqlite3 *g_db = conn_db(conn_id);
    if (!g_db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, conn_id ? "db: no open connection with that handle"
                                                     : "db: no open connection (call db.open first)");
        return res;
    }
    int before = sqlite3_total_changes(g_db);
    const char *p = skip_sql_blank(sql);
    int n = 0;
    while (*p) {
        sqlite3_stmt *stmt = NULL;
        const char *tail = NULL;
        n++;
        if (sqlite3_prepare_v2(g_db, p, -1, &stmt, &tail) != SQLITE_OK) {
            snprintf(g_errbuf, sizeof g_errbuf, "db.exec: statement %d: %s", n,
                     sqlite3_errmsg(g_db));
            res.is_err = 1; res.err = err_copy(DB_ERR_QUERY, g_errbuf);
            return res;
        }
        const char *next = skip_sql_blank(tail ? tail : p + strlen(p));
        if (n == 1 && params.len > 0 && *next) {
            sqlite3_finalize(stmt);
            res.is_err = 1;
            res.err = make_err(DB_ERR_QUERY,
                "db.exec: bind parameters with more than one statement are ambiguous; "
                "nothing was run -- pass one statement per call");
            return res;
        }
        if (stmt) {
            int rc = bind_and_step(stmt, params);
            /* A statement that returns rows (a pragma, a SELECT) is run to
             * completion so its side effects happen. */
            while (rc == SQLITE_ROW) rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);
            if (rc != SQLITE_DONE) {
                snprintf(g_errbuf, sizeof g_errbuf, "db.exec: statement %d: %s%s", n,
                         sqlite3_errmsg(g_db),
                         n > 1 ? " (the statements before it were applied)" : "");
                res.is_err = 1; res.err = err_copy(DB_ERR_QUERY, g_errbuf);
                return res;
            }
        }
        p = next;
    }
    res.ok = (uint64_t)(sqlite3_total_changes(g_db) - before);
    return res;
}

RowResult      db_one(const char *sql, StrArray params)  { return db_one_on(0, sql, params); }
RowArrayResult db_many(const char *sql, StrArray params) { return db_many_on(0, sql, params); }
U64Result      db_exec(const char *sql, StrArray params) { return db_exec_on(0, sql, params); }

/* ── metadata and result inspection (Story 29.2.2) ───────────────────────── */

U64Result db_last_insert_id(int conn_id)
{
    U64Result res = {0}; res.is_err = 0; res.ok = 0;
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
    U64Result res = {0}; res.is_err = 0; res.ok = 0;
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

/* Resolve conn_id to its sqlite3 handle: 0 is the default connection (the
 * lowest-numbered one still open), anything else must be an open id. */
static sqlite3 *conn_db(int64_t conn_id)
{
    if (g_nconns == 0) return NULL;
    if (conn_id == 0) return g_conns[0].db;
    for (int i = 0; i < g_nconns; i++)
        if (g_conns[i].id == conn_id) return g_conns[i].db;
    return NULL;
}

StmtResult db_prepare(int conn_id, const char *sql)
{
    StmtResult res = {0}; res.is_err = 0; res.ok = NULL;
    sqlite3 *db = conn_db(conn_id);
    if (!db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, "invalid conn_id");
        return res;
    }
    sqlite3_stmt *raw = NULL;
    if (sqlite3_prepare_v2(db, sql, -1, &raw, NULL) != SQLITE_OK) {
        res.is_err = 1;
        res.err = err_copy(DB_ERR_QUERY, sqlite3_errmsg(db));
        return res;
    }
    TkStmt *s = malloc(sizeof(TkStmt));
    if (!s) {   /* 121.21: was unchecked -- s->stmt through NULL */
        sqlite3_finalize(raw);
        res.is_err = 1;
        res.err = make_err(DB_ERR_QUERY, "out of memory");
        return res;
    }
    s->stmt = raw;
    s->db   = db;
    res.ok = s;
    return res;
}

BoolResult db_bind(TkStmt *stmt, StrArray params)
{
    BoolResult res = {0}; res.is_err = 0; res.ok = 1;
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
            res.err = err_copy(DB_ERR_QUERY, sqlite3_errmsg(stmt->db));
            return res;
        }
    }
    return res;
}

RowResult db_step(TkStmt *stmt)
{
    RowResult res = {0}; res.is_err = 0;
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
        res.err = err_copy(DB_ERR_QUERY, sqlite3_errmsg(stmt->db));
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
    BoolResult res = {0}; res.is_err = 0; res.ok = 1;
    sqlite3 *db = conn_db(conn_id);
    if (!db) {
        res.is_err = 1;
        res.err = make_err(DB_ERR_CONNECTION, "invalid conn_id");
        return res;
    }
    char *errmsg = NULL;
    if (sqlite3_exec(db, sql, NULL, NULL, &errmsg) != SQLITE_OK) {
        /* 121.21 (DAT-08): copy the message and give sqlite its allocation
         * back.  It was stored in the DbErr and never sqlite3_free'd, so
         * every failed BEGIN/COMMIT/ROLLBACK leaked it.  err_copy owns the
         * text (121.51 (5)). */
        snprintf(g_errbuf, sizeof g_errbuf, "%s", errmsg ? errmsg : sql);
        sqlite3_free(errmsg);
        res.is_err = 1;
        res.err = err_copy(DB_ERR_QUERY, g_errbuf);
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
    StrResult res = {0}; res.is_err = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = r.col_values[idx]; return res;
}

U64Result row_u64(Row r, const char *col)
{
    U64Result res = {0}; res.is_err = 0; res.ok = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = (uint64_t)strtoull(r.col_values[idx], NULL, 10); return res;
}

I64Result row_i64(Row r, const char *col)
{
    I64Result res = {0}; res.is_err = 0; res.ok = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = (int64_t)strtoll(r.col_values[idx], NULL, 10); return res;
}

F64Result row_f64(Row r, const char *col)
{
    F64Result res = {0}; res.is_err = 0; res.ok = 0.0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    res.ok = strtod(r.col_values[idx], NULL); return res;
}

BoolResult row_bool(Row r, const char *col)
{
    BoolResult res = {0}; res.is_err = 0; res.ok = 0;
    int idx = find_col(r, col);
    if (idx < 0) { res.is_err = 1; res.err = make_err(DB_ERR_NOT_FOUND, col); return res; }
    const char *v = r.col_values[idx];
    res.ok = (strcmp(v,"1")==0 || strcmp(v,"true")==0 || strcmp(v,"TRUE")==0) ? 1 : 0;
    return res;
}
