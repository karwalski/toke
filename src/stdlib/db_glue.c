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

/* tk_db_lastinsertid_w — return the last INSERT rowid.
 * conn is treated as the conn_id (0 = default global connection). */
int64_t tk_db_lastinsertid_w(int64_t conn) {
    U64Result res = db_last_insert_id((int)conn);
    if (res.is_err) return 0;
    return (int64_t)res.ok;
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

/* ── Query builder ───────────────────────────────────────────────────── */

typedef struct {
    char table[128];
    char fields[16][128];   /* field names */
    char values[16][256];   /* field values (string or int) */
    int  is_int[16];        /* 1 if value is integer */
    int  field_count;
} TkQueryBuilder;

/* tk_db_newquery_w — allocate a new empty query builder. */
int64_t tk_db_newquery_w(int64_t dummy) {
    (void)dummy;
    TkQueryBuilder *qb = (TkQueryBuilder *)calloc(1, sizeof(TkQueryBuilder));
    return (int64_t)(intptr_t)qb;
}

/* tk_db_settable_w — set the target table name on the query builder. */
int64_t tk_db_settable_w(int64_t q, int64_t t) {
    TkQueryBuilder *qb = (TkQueryBuilder *)(intptr_t)q;
    if (!qb || !t) return 0;
    snprintf(qb->table, sizeof qb->table, "%s", (const char *)(intptr_t)t);
    return q;
}

/* tk_db_setfield_w — add a string field=value pair.
 * f is a "field=value" string; split on the first '='. */
int64_t tk_db_setfield_w(int64_t q, int64_t f) {
    TkQueryBuilder *qb = (TkQueryBuilder *)(intptr_t)q;
    if (!qb || !f) return 0;
    if (qb->field_count >= 16) return q;
    const char *s = (const char *)(intptr_t)f;
    const char *eq = strchr(s, '=');
    int idx = qb->field_count;
    if (eq) {
        size_t klen = (size_t)(eq - s);
        if (klen >= sizeof qb->fields[0]) klen = sizeof qb->fields[0] - 1;
        memcpy(qb->fields[idx], s, klen);
        qb->fields[idx][klen] = '\0';
        snprintf(qb->values[idx], sizeof qb->values[0], "%s", eq + 1);
    } else {
        snprintf(qb->fields[idx], sizeof qb->fields[0], "%s", s);
        qb->values[idx][0] = '\0';
    }
    qb->is_int[idx] = 0;
    qb->field_count++;
    return q;
}

/* tk_db_setfieldint_w — add an integer field/value pair. */
int64_t tk_db_setfieldint_w(int64_t q, int64_t f, int64_t v) {
    TkQueryBuilder *qb = (TkQueryBuilder *)(intptr_t)q;
    if (!qb || !f) return 0;
    if (qb->field_count >= 16) return q;
    int idx = qb->field_count;
    snprintf(qb->fields[idx], sizeof qb->fields[0], "%s",
             (const char *)(intptr_t)f);
    snprintf(qb->values[idx], sizeof qb->values[0], "%lld", (long long)v);
    qb->is_int[idx] = 1;
    qb->field_count++;
    return q;
}

/* tk_db_buildinsert_w — generate an INSERT SQL string from the builder.
 * Returns a heap-allocated SQL string. */
int64_t tk_db_buildinsert_w(int64_t q) {
    TkQueryBuilder *qb = (TkQueryBuilder *)(intptr_t)q;
    if (!qb || qb->field_count == 0 || qb->table[0] == '\0') return 0;
    /* Estimate buffer size */
    char *buf = (char *)malloc(4096);
    if (!buf) return 0;
    int off = snprintf(buf, 4096, "INSERT INTO %s (", qb->table);
    for (int i = 0; i < qb->field_count; i++) {
        if (i > 0) off += snprintf(buf + off, 4096 - off, ", ");
        off += snprintf(buf + off, 4096 - off, "%s", qb->fields[i]);
    }
    off += snprintf(buf + off, 4096 - off, ") VALUES (");
    for (int i = 0; i < qb->field_count; i++) {
        if (i > 0) off += snprintf(buf + off, 4096 - off, ", ");
        if (qb->is_int[i])
            off += snprintf(buf + off, 4096 - off, "%s", qb->values[i]);
        else
            off += snprintf(buf + off, 4096 - off, "'%s'", qb->values[i]);
    }
    off += snprintf(buf + off, 4096 - off, ")");
    return (int64_t)(intptr_t)buf;
}

/* tk_db_buildupdate_w — generate an UPDATE SQL string from the builder.
 * The first field is used as the WHERE clause (field=value). */
int64_t tk_db_buildupdate_w(int64_t q) {
    TkQueryBuilder *qb = (TkQueryBuilder *)(intptr_t)q;
    if (!qb || qb->field_count < 2 || qb->table[0] == '\0') return 0;
    char *buf = (char *)malloc(4096);
    if (!buf) return 0;
    int off = snprintf(buf, 4096, "UPDATE %s SET ", qb->table);
    for (int i = 1; i < qb->field_count; i++) {
        if (i > 1) off += snprintf(buf + off, 4096 - off, ", ");
        if (qb->is_int[i])
            off += snprintf(buf + off, 4096 - off, "%s = %s",
                            qb->fields[i], qb->values[i]);
        else
            off += snprintf(buf + off, 4096 - off, "%s = '%s'",
                            qb->fields[i], qb->values[i]);
    }
    /* First field is the WHERE key */
    if (qb->is_int[0])
        off += snprintf(buf + off, 4096 - off, " WHERE %s = %s",
                        qb->fields[0], qb->values[0]);
    else
        off += snprintf(buf + off, 4096 - off, " WHERE %s = '%s'",
                        qb->fields[0], qb->values[0]);
    return (int64_t)(intptr_t)buf;
}

/* tk_db_qexecute_w — build INSERT SQL from the query builder and execute it.
 * Returns affected row count or 0 on error. */
int64_t tk_db_qexecute_w(int64_t q) {
    int64_t sql = tk_db_buildinsert_w(q);
    if (!sql) return 0;
    StrArray sa = { NULL, 0 };
    U64Result res = db_exec((const char *)(intptr_t)sql, sa);
    free((void *)(intptr_t)sql);
    if (res.is_err) return 0;
    return res.ok > 0 ? (int64_t)res.ok : 1;
}

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

/* ── Linker-gap additions ───────────────────────────────────────────────── */

/* db.execparams(sql, params) — execute with explicit params array */
int64_t tk_db_execparams_w(int64_t sql, int64_t params) {
    return tk_db_exec_w(sql, params);
}

/* db.manyparams(sql, params) — query many with explicit params */
int64_t tk_db_manyparams_w(int64_t sql, int64_t params) {
    return tk_db_many_w(sql, params);
}

/* db.queryone(sql) — query single row, no params */
int64_t tk_db_queryone_w(int64_t sql) {
    return tk_db_one_w(sql, 0);
}

/* db.queryoneparams(sql, params) — query single row with params */
int64_t tk_db_queryoneparams_w(int64_t sql, int64_t params) {
    return tk_db_one_w(sql, params);
}

/* db.queryparams(sql, params) — query many with params (alias) */
int64_t tk_db_queryparams_w(int64_t sql, int64_t params) {
    return tk_db_many_w(sql, params);
}

/* db.rowbool(row, col) — alias for row_bool */
int64_t tk_db_rowbool_w(int64_t row, int64_t col) {
    return tk_row_bool_w(row, col);
}

/* db.rowget(row, col) — alias for row_str (get field as string) */
int64_t tk_db_rowget_w(int64_t row, int64_t col) {
    return tk_row_str_w(row, col);
}

/* db.rowi32(row, col) — get int field (same as i64 at ABI level) */
int64_t tk_db_rowi32_w(int64_t row, int64_t col) {
    return tk_row_i64_w(row, col);
}

/* db.rowi64(row, col) — alias for row_i64 */
int64_t tk_db_rowi64_w(int64_t row, int64_t col) {
    return tk_row_i64_w(row, col);
}

/* db.rowslen(rows) — count of rows in result array */
int64_t tk_db_rowslen_w(int64_t rows) {
    return tk_db_rows_w(rows);
}

/* db.rowstr(row, col) — alias for row_str */
int64_t tk_db_rowstr_w(int64_t row, int64_t col) {
    return tk_row_str_w(row, col);
}

/* db.nowms() — convenience: current time in ms (delegates to time module) */
int64_t tk_db_nowms_w(int64_t dummy) {
    (void)dummy;
    extern int64_t tk_time_nowms_w(int64_t);
    return tk_time_nowms_w(0);
}
