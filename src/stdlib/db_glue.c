/*
 * db_glue.c — i64-ABI wrappers for std.db module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.db.
 */

#include "db.h"
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "tk_runtime.h" /* 138.6: tk_current_error + tk_err_box (127.109) */
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

/*
 * 138.6 — the error channel.  Every fallible wrapper below keeps
 * @tk_current_error truthful on every return, the way json_glue.c does
 * (127.193): 0 on success, and on failure a {tag, msg} box whose tag is the
 * DbErr variant index ($connection, $query, $notfound, $constraint -- the
 * order of stdlib/db.tki and of DbErrKind).  The returned value still keeps
 * the 0/null sentinel on failure, so a compiler that decides these calls by
 * value keeps working; one that lists the wrapper in llvm.c's
 * is_stdlib_slot_wrapper() decides by the slot, which is what makes a stored
 * 0, 0.0 or false read as a value rather than as $err (loke T-22, N-5).
 */
static int64_t db_raise(DbErr e) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)e.kind;
    box[1] = (int64_t)(intptr_t)(e.msg ? e.msg : "");
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}
static int64_t db_raise_msg(DbErrKind kind, const char *msg) {
    DbErr e; e.kind = kind; e.msg = msg; return db_raise(e);
}

/* 125.1 / 138.6: db.open(dsn) -> u64!DbErr.  Returns the new connection's
 * handle (>= 1, a fresh one on every call) or 0 on failure.  db_connect is
 * capability-gated (fs.read + fs.write). */
int64_t tk_db_open_w(int64_t dsn) {
    if (!dsn) return 0;
    int64_t id = db_connect((const char *)(intptr_t)dsn);
    return id > 0 ? id : 0;
}

/* 125.1: db.close() closes the default connection (the lowest-numbered one
 * still open). */
int64_t tk_db_close_w(void) {
    db_close();
    return 1;
}

/* 138.6: db.closeon(h) -> bool: true if h was open and is now closed. */
int64_t tk_db_closeon_w(int64_t h) {
    if (h <= 0) return 0;
    return (int64_t)db_close_conn(h);
}

/* 138.6: a handle of 0 would silently mean "the default connection" to
 * db.c; from source it is always a mistake (db.open never returns 0 on
 * success), so the *on wrappers refuse it. */
static int64_t exec_on(int64_t h, int64_t sql, int64_t params) {
    if (!sql) return db_raise_msg(DB_ERR_QUERY, "db.exec: null sql");
    StrArray sa = toke_arr_to_strarray(params);
    U64Result res = db_exec_on(h, (const char *)(intptr_t)sql, sa);
    free((void *)sa.data);
    if (res.is_err) return db_raise(res.err);
    tk_current_error = 0;
    /* 138.6: llvm.c lists tk_db_exec_w/tk_db_execon_w as slot wrappers, so
     * `mt` discriminates on tk_current_error (cleared above) and a count of
     * 0 — every CREATE, an UPDATE that matched nothing — is $ok 0. */
    return (int64_t)res.ok;
}

int64_t tk_db_exec_w(int64_t sql, int64_t params) {
    return exec_on(0, sql, params);
}

int64_t tk_db_execon_w(int64_t h, int64_t sql, int64_t params) {
    if (h <= 0) return db_raise_msg(DB_ERR_CONNECTION, "db.execon: not a connection handle");
    return exec_on(h, sql, params);
}

/* ── 135.30: $row is a TkRowRec {cols:[[str]]}, not the C Row ─────────── */

/* row_rec — a C Row as the $row record; the strings are shared with it. */
static int64_t row_rec(const Row *r) {
    TkRowRec *rec = (TkRowRec *)malloc(sizeof *rec);
    int64_t cols = tk_arr_alloc((int64_t)r->col_count, (int64_t)r->col_count);
    if (!rec || !cols) {
        free(rec);
        if (cols) free((int64_t *)(intptr_t)cols - TK_ARR_HDR);
        return 0;
    }
    for (uint64_t c = 0; c < r->col_count; c++) {
        int64_t pair = tk_arr_alloc(2, 2);
        if (!pair) { tk_arr_setlen(cols, (int64_t)c); break; }
        ((int64_t *)(intptr_t)pair)[0] = (int64_t)(intptr_t)(r->col_names[c] ? r->col_names[c] : "");
        ((int64_t *)(intptr_t)pair)[1] = (int64_t)(intptr_t)(r->col_values[c] ? r->col_values[c] : "");
        ((int64_t *)(intptr_t)cols)[c] = pair;
    }
    rec->cols = cols;
    return (int64_t)(intptr_t)rec;
}

/* row_drop_index — free a C Row's arrays once row_rec has taken its
 * strings (the record now owns them). */
static void row_drop_index(Row *r) {
    free((void *)r->col_names);
    free((void *)r->col_values);
    free(r->col_nulls);
}

/* row_view — a C Row reading a $row record (built by db.one/db.many or a
 * toke literal).  Its name/value arrays are malloc'd and the strings
 * borrowed; release with row_view_free.  A pair with fewer than two
 * elements reads "" for what it lacks. */
static Row row_view(int64_t rec_h) {
    Row r; r.col_names = NULL; r.col_values = NULL; r.col_nulls = NULL; r.col_count = 0;
    const TkRowRec *rec = (const TkRowRec *)(intptr_t)rec_h;
    int64_t n = rec ? tk_arr_len(rec->cols) : 0;
    if (n <= 0) return r;
    const char **names  = (const char **)malloc((size_t)n * sizeof(char *));
    const char **values = (const char **)malloc((size_t)n * sizeof(char *));
    if (!names || !values) { free((void *)names); free((void *)values); return r; }
    const int64_t *cols = (const int64_t *)(intptr_t)rec->cols;
    for (int64_t c = 0; c < n; c++) {
        int64_t pl = tk_arr_len(cols[c]);
        const int64_t *pair = (const int64_t *)(intptr_t)cols[c];
        names[c]  = (pl >= 1 && pair[0]) ? (const char *)(intptr_t)pair[0] : "";
        values[c] = (pl >= 2 && pair[1]) ? (const char *)(intptr_t)pair[1] : "";
    }
    r.col_names = names; r.col_values = values; r.col_count = (uint64_t)n;
    return r;
}

static void row_view_free(Row *r) {
    free((void *)r->col_names);
    free((void *)r->col_values);
}

static int64_t one_on(int64_t h, int64_t sql, int64_t params) {
    if (!sql) return 0;
    StrArray sa = toke_arr_to_strarray(params);
    RowResult res = db_one_on(h, (const char *)(intptr_t)sql, sa);
    free((void *)sa.data);
    if (res.is_err) return 0;
    int64_t rec = row_rec(&res.ok);
    if (rec) row_drop_index(&res.ok);
    return rec;
}

int64_t tk_db_one_w(int64_t sql, int64_t params) {
    return one_on(0, sql, params);
}

int64_t tk_db_oneon_w(int64_t h, int64_t sql, int64_t params) {
    if (h <= 0) return 0;
    return one_on(h, sql, params);
}

static int64_t many_on(int64_t h, int64_t sql, int64_t params) {
    if (!sql) return 0;
    StrArray sa = toke_arr_to_strarray(params);
    RowArrayResult res = db_many_on(h, (const char *)(intptr_t)sql, sa);
    free((void *)sa.data);
    if (res.is_err) return 0;
    uint64_t count = res.ok.len;
    int64_t out = tk_arr_alloc((int64_t)count, (int64_t)count);
    if (!out) return 0;
    int64_t *block = (int64_t *)(intptr_t)out;
    for (uint64_t i = 0; i < count; i++) {
        int64_t rec = row_rec(&res.ok.data[i]);
        if (!rec) {
            tk_arr_setlen(out, (int64_t)i);
            break;
        }
        row_drop_index(&res.ok.data[i]);
        block[i] = rec;
    }
    free(res.ok.data);
    return out;
}

int64_t tk_db_many_w(int64_t sql, int64_t params) {
    return many_on(0, sql, params);
}

int64_t tk_db_manyon_w(int64_t h, int64_t sql, int64_t params) {
    if (h <= 0) return 0;
    return many_on(h, sql, params);
}

int64_t tk_db_tableexists_w(int64_t conn, int64_t table) {
    if (!table) return 0;
    return (int64_t)db_table_exists((int)conn, (const char *)(intptr_t)table);
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

/* tk_db_getfield_w — get a field value by column name from a $row. */
int64_t tk_db_getfield_w(int64_t row, int64_t name) {
    if (!row || !name) return 0;
    Row r = row_view(row);
    StrResult res = row_str(r, (const char *)(intptr_t)name);
    row_view_free(&r);
    if (res.is_err) return 0;
    return (int64_t)(intptr_t)res.ok;
}

/* tk_db_print_w — print all rows to stdout for debugging. */
int64_t tk_db_print_w(int64_t rows) {
    if (!rows) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)rows;
    int64_t count = ptr[-1];
    for (int64_t i = 0; i < count; i++) {
        if (!ptr[i]) continue;
        Row r = row_view(ptr[i]);
        for (uint64_t c = 0; c < r.col_count; c++) {
            if (c > 0) printf("\t");
            printf("%s=%s", r.col_names[c], r.col_values[c]);
        }
        row_view_free(&r);
        printf("\n");
    }
    return 0;
}

/* row accessor wrappers.  138.6: the scalar ones (i64, u64, f64, bool) keep
 * the error slot truthful -- see db_raise above.  row.str's value is a
 * pointer, never 0 on success (an empty column is ""), so the sentinel
 * already tells it apart; it sets the slot too, for one rule everywhere. */
int64_t tk_row_str_w(int64_t row, int64_t col) {
    if (!row || !col) return db_raise_msg(DB_ERR_NOT_FOUND, "row.str: null row or column name");
    Row r = row_view(row);
    StrResult res = row_str(r, (const char *)(intptr_t)col);
    row_view_free(&r);
    if (res.is_err) return db_raise(res.err);
    tk_current_error = 0;
    return (int64_t)(intptr_t)res.ok;
}

int64_t tk_row_i64_w(int64_t row, int64_t col) {
    if (!row || !col) return db_raise_msg(DB_ERR_NOT_FOUND, "row.i64: null row or column name");
    Row r = row_view(row);
    I64Result res = row_i64(r, (const char *)(intptr_t)col);
    row_view_free(&r);
    if (res.is_err) return db_raise(res.err);
    tk_current_error = 0;
    return res.ok;
}

int64_t tk_row_u64_w(int64_t row, int64_t col) {
    if (!row || !col) return db_raise_msg(DB_ERR_NOT_FOUND, "row.u64: null row or column name");
    Row r = row_view(row);
    U64Result res = row_u64(r, (const char *)(intptr_t)col);
    row_view_free(&r);
    if (res.is_err) return db_raise(res.err);
    tk_current_error = 0;
    return (int64_t)res.ok;
}

int64_t tk_row_f64_w(int64_t row, int64_t col) {
    if (!row || !col) return db_raise_msg(DB_ERR_NOT_FOUND, "row.f64: null row or column name");
    Row r = row_view(row);
    F64Result res = row_f64(r, (const char *)(intptr_t)col);
    row_view_free(&r);
    if (res.is_err) return db_raise(res.err);
    tk_current_error = 0;
    return f64_to_i64(res.ok);
}

int64_t tk_row_bool_w(int64_t row, int64_t col) {
    if (!row || !col) return db_raise_msg(DB_ERR_NOT_FOUND, "row.bool: null row or column name");
    Row r = row_view(row);
    BoolResult res = row_bool(r, (const char *)(intptr_t)col);
    row_view_free(&r);
    if (res.is_err) return db_raise(res.err);
    tk_current_error = 0;
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
int64_t tk_db_nowms_w(void) {
    extern int64_t tk_time_nowms_w(void);
    return tk_time_nowms_w();
}
