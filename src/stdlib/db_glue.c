/*
 * db_glue.c — i64-ABI wrappers for std.db module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.db.
 */

#include "db.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#ifndef TK_STRARRAY_DEFINED
#define TK_STRARRAY_DEFINED
typedef struct { const char **data; uint64_t len; } StrArray;
#endif

/* f64<->i64 bitcast helpers */
static int64_t f64_to_i64(double d) {
    int64_t i;
    memcpy(&i, &d, sizeof(i));
    return i;
}

/*
 * Unpack a toke-format array of strings into a StrArray.
 */
static StrArray toke_arr_to_strarray(int64_t arr_i64) {
    StrArray sa;
    if (!arr_i64) { sa.data = NULL; sa.len = 0; return sa; }
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    if (len <= 0) { sa.data = NULL; sa.len = 0; return sa; }
    sa.data = (const char **)malloc((size_t)len * sizeof(const char *));
    if (!sa.data) { sa.len = 0; return sa; }
    for (int64_t i = 0; i < len; i++)
        sa.data[i] = (const char *)(intptr_t)ptr[i];
    sa.len = (uint64_t)len;
    return sa;
}

int64_t tk_db_open_w(int64_t dsn) {
    if (!dsn) return -1;
    return (int64_t)db_open((const char *)(intptr_t)dsn);
}

int64_t tk_db_close_w(int64_t conn) {
    (void)conn;
    db_close();
    return 0;
}

int64_t tk_db_exec_w(int64_t sql, int64_t params) {
    if (!sql) return 0;
    StrArray sa = toke_arr_to_strarray(params);
    U64Result res = db_exec((const char *)(intptr_t)sql, sa);
    free((void *)sa.data);
    if (res.is_err) return 0;
    return res.ok > 0 ? (int64_t)res.ok : 1;
}

int64_t tk_db_one_w(int64_t sql, int64_t params) {
    if (!sql) return 0;
    StrArray sa = toke_arr_to_strarray(params);
    RowResult res = db_one((const char *)(intptr_t)sql, sa);
    free((void *)sa.data);
    if (res.is_err) return 0;
    Row *heap_row = (Row *)malloc(sizeof(Row));
    if (!heap_row) return 0;
    *heap_row = res.ok;
    return (int64_t)(intptr_t)heap_row;
}

int64_t tk_db_many_w(int64_t sql, int64_t params) {
    if (!sql) return 0;
    StrArray sa = toke_arr_to_strarray(params);
    RowArrayResult res = db_many((const char *)(intptr_t)sql, sa);
    free((void *)sa.data);
    if (res.is_err) return 0;
    uint64_t count = res.ok.len;
    int64_t *block = (int64_t *)malloc((count + 1) * sizeof(int64_t));
    if (!block) return 0;
    block[0] = (int64_t)count;
    for (uint64_t i = 0; i < count; i++) {
        Row *heap_row = (Row *)malloc(sizeof(Row));
        if (!heap_row) {
            block[0] = (int64_t)i;
            break;
        }
        *heap_row = res.ok.data[i];
        block[i + 1] = (int64_t)(intptr_t)heap_row;
    }
    free(res.ok.data);
    return (int64_t)(intptr_t)(block + 1);
}

int64_t tk_db_tableexists_w(int64_t conn, int64_t table) {
    if (!table) return 0;
    return (int64_t)db_table_exists((int)conn, (const char *)(intptr_t)table);
}

/* ── Legacy API wrappers ─────────────────────────────────────────────── */

/* Helper: empty params for legacy functions that don't take parameters */
static StrArray empty_params(void) {
    StrArray sa; sa.data = NULL; sa.len = 0; return sa;
}

int64_t tk_db_connect_w(int64_t dsn) { return tk_db_open_w(dsn); }

/* tk_db_query_w — legacy query; maps to db_many() with empty params.
 * Returns toke-format array of Row pointers (block[-1]=count). */
int64_t tk_db_query_w(int64_t conn, int64_t sql) {
    (void)conn;
    if (!sql) return 0;
    StrArray sa = empty_params();
    RowArrayResult res = db_many((const char *)(intptr_t)sql, sa);
    if (res.is_err) return 0;
    uint64_t count = res.ok.len;
    int64_t *block = (int64_t *)malloc((count + 1) * sizeof(int64_t));
    if (!block) { free(res.ok.data); return 0; }
    block[0] = (int64_t)count;
    for (uint64_t i = 0; i < count; i++) {
        Row *heap_row = (Row *)malloc(sizeof(Row));
        if (!heap_row) { block[0] = (int64_t)i; break; }
        *heap_row = res.ok.data[i];
        block[i + 1] = (int64_t)(intptr_t)heap_row;
    }
    free(res.ok.data);
    return (int64_t)(intptr_t)(block + 1);
}

/* tk_db_insert_w — legacy insert; maps to db_exec() with empty params. */
int64_t tk_db_insert_w(int64_t conn, int64_t sql) {
    (void)conn;
    if (!sql) return 0;
    StrArray sa = empty_params();
    U64Result res = db_exec((const char *)(intptr_t)sql, sa);
    if (res.is_err) return 0;
    return res.ok > 0 ? (int64_t)res.ok : 1;
}

/* tk_db_delete_w — legacy delete; maps to db_exec() with empty params. */
int64_t tk_db_delete_w(int64_t conn, int64_t sql) {
    (void)conn;
    if (!sql) return 0;
    StrArray sa = empty_params();
    U64Result res = db_exec((const char *)(intptr_t)sql, sa);
    if (res.is_err) return 0;
    return res.ok > 0 ? (int64_t)res.ok : 1;
}

/* tk_db_execute_w — legacy execute; maps to db_exec() with empty params. */
int64_t tk_db_execute_w(int64_t conn, int64_t q) {
    (void)conn;
    if (!q) return 0;
    StrArray sa = empty_params();
    U64Result res = db_exec((const char *)(intptr_t)q, sa);
    if (res.is_err) return 0;
    return res.ok > 0 ? (int64_t)res.ok : 1;
}

/* tk_db_lastinsertid_w — not tracked in current single-connection API.
 * Returns 0 (no reliable last-insert-id without a conn_id). */
int64_t tk_db_lastinsertid_w(int64_t conn) {
    (void)conn;
    return 0; /* TODO: wire to db_last_insert_id() when conn_id mapping exists */
}

/* ── Result accessor wrappers for toke-format row arrays ─────────────── */

/* tk_db_rows_w — return count of rows in a toke-format result array. */
int64_t tk_db_rows_w(int64_t result) {
    if (!result) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)result;
    return ptr[-1];
}

/* tk_db_getrow_w — return the Row pointer at index idx from a result array. */
int64_t tk_db_getrow_w(int64_t result, int64_t idx) {
    if (!result) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)result;
    int64_t count = ptr[-1];
    if (idx < 0 || idx >= count) return 0;
    return ptr[idx];
}

/* tk_db_getfield_w — get a field value by column name from a Row pointer. */
int64_t tk_db_getfield_w(int64_t row, int64_t name) {
    if (!row || !name) return 0;
    Row *rp = (Row *)(intptr_t)row;
    StrResult res = row_str(*rp, (const char *)(intptr_t)name);
    if (res.is_err) return 0;
    return (int64_t)(intptr_t)res.ok;
}

/* tk_db_print_w — print all rows to stdout for debugging. */
int64_t tk_db_print_w(int64_t rows) {
    if (!rows) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)rows;
    int64_t count = ptr[-1];
    for (int64_t i = 0; i < count; i++) {
        Row *rp = (Row *)(intptr_t)ptr[i];
        if (!rp) continue;
        for (uint64_t c = 0; c < rp->col_count; c++) {
            if (c > 0) printf("\t");
            printf("%s=%s", rp->col_names[c] ? rp->col_names[c] : "",
                            rp->col_values[c] ? rp->col_values[c] : "NULL");
        }
        printf("\n");
    }
    return 0;
}

/* ── Query builder stubs (no C implementation exists) ────────────────── */
int64_t tk_db_newquery_w(int64_t dummy) { (void)dummy; return 0; /* TODO: no query builder in C API */ }
int64_t tk_db_settable_w(int64_t q, int64_t t) { (void)q; (void)t; return 0; /* TODO: no query builder in C API */ }
int64_t tk_db_setfield_w(int64_t q, int64_t f) { (void)q; (void)f; return 0; /* TODO: no query builder in C API */ }
int64_t tk_db_setfieldint_w(int64_t q, int64_t f, int64_t v) { (void)q; (void)f; (void)v; return 0; /* TODO: no query builder in C API */ }

/* row accessor wrappers */
int64_t tk_row_str_w(int64_t row, int64_t col) {
    if (!row || !col) return 0;
    Row *rp = (Row *)(intptr_t)row;
    StrResult res = row_str(*rp, (const char *)(intptr_t)col);
    if (res.is_err) return 0;
    return (int64_t)(intptr_t)res.ok;
}

int64_t tk_row_i64_w(int64_t row, int64_t col) {
    if (!row || !col) return 0;
    Row *rp = (Row *)(intptr_t)row;
    I64Result res = row_i64(*rp, (const char *)(intptr_t)col);
    if (res.is_err) return 0;
    return res.ok;
}

int64_t tk_row_u64_w(int64_t row, int64_t col) {
    if (!row || !col) return 0;
    Row *rp = (Row *)(intptr_t)row;
    U64Result res = row_u64(*rp, (const char *)(intptr_t)col);
    if (res.is_err) return 0;
    return (int64_t)res.ok;
}

int64_t tk_row_f64_w(int64_t row, int64_t col) {
    if (!row || !col) return 0;
    Row *rp = (Row *)(intptr_t)row;
    F64Result res = row_f64(*rp, (const char *)(intptr_t)col);
    if (res.is_err) return 0;
    return f64_to_i64(res.ok);
}

int64_t tk_row_bool_w(int64_t row, int64_t col) {
    if (!row || !col) return 0;
    Row *rp = (Row *)(intptr_t)row;
    BoolResult res = row_bool(*rp, (const char *)(intptr_t)col);
    if (res.is_err) return 0;
    return (int64_t)res.ok;
}
