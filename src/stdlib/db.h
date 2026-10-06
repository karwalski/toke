/* SPDX-License-Identifier: MIT */
#ifndef TK_STDLIB_DB_H
#define TK_STDLIB_DB_H

/*
 * db.h — C interface for the std.db standard library module.
 *
 * Type mappings:
 *   Row      = struct { col_names, col_values: const char**; col_count: u64 }
 *   [Row]    = RowArray
 *   DbErr    = struct { DbErrKind kind; const char *msg; }
 *   Row!DbErr      = RowResult
 *   [Row]!DbErr    = RowArrayResult
 *   u64!DbErr      = U64Result
 *   Str!DbErr      = StrResult
 *   i64!DbErr      = I64Result
 *   f64!DbErr      = F64Result
 *   bool!DbErr     = BoolResult
 *
 * Story: 1.3.3  Branch: feature/stdlib-db
 */

#include <stdint.h>

/* ── basic aggregates ─────────────────────────────────────────────────────── */

#ifndef TK_STRARRAY_DEFINED
#define TK_STRARRAY_DEFINED
typedef struct { const char **data; uint64_t len; } StrArray;
#endif

typedef struct {
    const char **col_names;
    const char **col_values;
    int         *col_nulls;   /* 1 if the column value was SQL NULL, else 0 */
    uint64_t     col_count;
} Row;

typedef struct { Row *data; uint64_t len; } RowArray;

/*
 * TkRowRec — the $row record toke sees (135.30).  tkc reads `r.cols` as
 * slot 0 of db.tki's Row {cols:[[str]]}: a toke array with one @(name;
 * value) pair per column (value "" for SQL NULL, as row.str gives it).
 * db.one/db.many hand these out instead of the C Row above, whose slot 0
 * is the col_names pointer (`r.cols.len` read 0 or garbage), and every
 * row.* accessor reads the record, so a `$row{cols:@(...)}` literal -- as
 * in db.md's own error arms -- is read the same way.  `make
 * check-tki-layout` keeps it in step with the .tki.
 */
/* tki-record: db.Row */
typedef struct {
    int64_t cols;       /* [[str]]: @(name; value) per column */
} TkRowRec;

/* ── error sum type ───────────────────────────────────────────────────────── */

typedef enum {
    DB_ERR_CONNECTION,
    DB_ERR_QUERY,
    DB_ERR_NOT_FOUND,
    DB_ERR_CONSTRAINT
} DbErrKind;

typedef struct { DbErrKind kind; const char *msg; } DbErr;

/* ── result types ─────────────────────────────────────────────────────────── */

typedef struct { Row        ok; int is_err; DbErr err; } RowResult;
typedef struct { RowArray   ok; int is_err; DbErr err; } RowArrayResult;
typedef struct { uint64_t   ok; int is_err; DbErr err; } U64Result;
typedef struct { const char*ok; int is_err; DbErr err; } StrResult;
typedef struct { int64_t    ok; int is_err; DbErr err; } I64Result;
typedef struct { double     ok; int is_err; DbErr err; } F64Result;
typedef struct { int        ok; int is_err; DbErr err; } BoolResult;

/* ── opaque connection handle ─────────────────────────────────────────────── */

typedef struct DbConn DbConn;

/* ── connection lifecycle ─────────────────────────────────────────────────── */

/* 138.6: several connections.  db_connect returns a fresh handle (>= 1,
 * never reused) or -1; conn_id 0 names the default connection, the
 * lowest-numbered one still open.  db_open/db_close are the handle-less
 * forms (db_open: 0 on success). */
int64_t db_connect(const char *dsn);
int     db_open(const char *dsn);
int     db_close_conn(int64_t conn_id);   /* 1 if a connection was closed */
void    db_close(void);

/* ── query functions ──────────────────────────────────────────────────────── */

RowResult      db_one(const char *sql,  StrArray params);
RowArrayResult db_many(const char *sql, StrArray params);
U64Result      db_exec(const char *sql, StrArray params);
RowResult      db_one_on(int64_t conn_id, const char *sql,  StrArray params);
RowArrayResult db_many_on(int64_t conn_id, const char *sql, StrArray params);
U64Result      db_exec_on(int64_t conn_id, const char *sql, StrArray params);

/* ── row accessor functions ───────────────────────────────────────────────── */

StrResult  row_str(Row r,  const char *col);
U64Result  row_u64(Row r,  const char *col);
I64Result  row_i64(Row r,  const char *col);
F64Result  row_f64(Row r,  const char *col);
BoolResult row_bool(Row r, const char *col);

/* ── metadata and result inspection (Story 29.2.2) ───────────────────────── */

U64Result  db_last_insert_id(int conn_id);           /* last INSERT rowid      */
U64Result  db_affected_rows(int conn_id);            /* rows affected by last stmt */
StrArray   db_columns(Row r);                        /* column names from a Row */
int        db_is_null(Row r, const char *col);       /* 1 if value is NULL/missing */
int        db_table_exists(int conn_id, const char *name); /* 1 if table exists */

/* ── prepared statements ──────────────────────────────────────────────────── */

typedef struct TkStmt TkStmt;
typedef struct { TkStmt *ok; int is_err; DbErr err; } StmtResult;

StmtResult db_prepare(int conn_id, const char *sql);
BoolResult db_bind(TkStmt *stmt, StrArray params);
RowResult  db_step(TkStmt *stmt);
void       db_finalize(TkStmt *stmt);

/* ── transactions ─────────────────────────────────────────────────────────── */

BoolResult db_begin(int conn_id);
BoolResult db_commit(int conn_id);
BoolResult db_rollback(int conn_id);

/* ── multi-backend vtable (Story 57.10) ──────────────────────────────────── */

typedef struct DbBackend {
    const char *name;
    int  (*open)(const char *dsn);
    void (*close)(void);
    RowResult      (*one)(const char *sql, StrArray params);
    RowArrayResult (*many)(const char *sql, StrArray params);
    U64Result      (*exec)(const char *sql, StrArray params);
    StmtResult     (*prepare)(int conn_id, const char *sql);
    BoolResult     (*bind)(TkStmt *stmt, StrArray params);
    RowResult      (*step)(TkStmt *stmt);
    void           (*finalize)(TkStmt *stmt);
    BoolResult     (*begin)(int conn_id);
    BoolResult     (*commit)(int conn_id);
    BoolResult     (*rollback)(int conn_id);
    U64Result      (*last_insert_id)(int conn_id);
    U64Result      (*affected_rows)(int conn_id);
    int            (*table_exists)(int conn_id, const char *name);
} DbBackend;

/* Backend declarations — available when compiled with the corresponding flag */
#ifdef TK_HAVE_LIBPQ
extern const DbBackend db_postgres_backend;
#endif
#ifdef TK_HAVE_MYSQL
extern const DbBackend db_mysql_backend;
#endif

#endif /* TK_STDLIB_DB_H */
