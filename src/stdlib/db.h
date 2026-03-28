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

typedef struct { const char **data; uint64_t len; } StrArray;

typedef struct {
    const char **col_names;
    const char **col_values;
    uint64_t     col_count;
} Row;

typedef struct { Row *data; uint64_t len; } RowArray;

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

int  db_open(const char *dsn);
void db_close(void);

/* ── query functions ──────────────────────────────────────────────────────── */

RowResult      db_one(const char *sql,  StrArray params);
RowArrayResult db_many(const char *sql, StrArray params);
U64Result      db_exec(const char *sql, StrArray params);

/* ── row accessor functions ───────────────────────────────────────────────── */

StrResult  row_str(Row r,  const char *col);
U64Result  row_u64(Row r,  const char *col);
I64Result  row_i64(Row r,  const char *col);
F64Result  row_f64(Row r,  const char *col);
BoolResult row_bool(Row r, const char *col);

#endif /* TK_STDLIB_DB_H */
