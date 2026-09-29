/* db_mysql.c — MySQL/MariaDB backend for std.db (Story 57.10.3)
 * Compile with: -DTK_HAVE_MYSQL $(mysql_config --cflags --libs) */

#ifdef TK_HAVE_MYSQL

#include "db.h"
#include "capabilities.h"   /* 124.4c2: mysql connect is a net sink */
#include "tk_secure.h"      /* 121.21: wipe the DSN password copy */
#include <mysql/mysql.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static MYSQL *g_mysql = NULL;

static DbErr my_err(DbErrKind kind, const char *msg)
{
    DbErr e; e.kind = kind; e.msg = msg; return e;
}

/* 121.51 (5): a DbErr owns its text -- the driver's error buffer is
 * overwritten by the next call on the connection and freed when it closes,
 * so e.msg read later showed another error or freed memory.  Driver messages
 * go through here: a heap copy, never freed, like every other str toke holds. */
static DbErr my_err_copy(DbErrKind kind, const char *msg)
{
    char *c = strdup(msg ? msg : "");
    return my_err(kind, c ? c : "db: out of memory copying the error message");
}

/* Parse mysql://user:pass@host:port/dbname */
static int parse_mysql_dsn(const char *dsn,
    char *host, size_t hl, int *port,
    char *user, size_t ul, char *pass, size_t pl,
    char *db, size_t dl)
{
    const char *p = dsn;
    if (strncmp(p, "mysql://", 8) == 0) p += 8;

    /* user:pass@ */
    const char *at = strchr(p, '@');
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        if (colon) {
            snprintf(user, ul, "%.*s", (int)(colon - p), p);
            snprintf(pass, pl, "%.*s", (int)(at - colon - 1), colon + 1);
        } else {
            snprintf(user, ul, "%.*s", (int)(at - p), p);
            pass[0] = '\0';
        }
        p = at + 1;
    } else {
        user[0] = '\0'; pass[0] = '\0';
    }

    /* host:port/db */
    const char *slash = strchr(p, '/');
    const char *colon2 = strchr(p, ':');
    if (colon2 && (!slash || colon2 < slash)) {
        snprintf(host, hl, "%.*s", (int)(colon2 - p), p);
        *port = atoi(colon2 + 1);
    } else {
        if (slash)
            snprintf(host, hl, "%.*s", (int)(slash - p), p);
        else
            snprintf(host, hl, "%s", p);
        *port = 3306;
    }
    if (slash)
        snprintf(db, dl, "%s", slash + 1);
    else
        db[0] = '\0';

    return 0;
}

static int my_open(const char *dsn)
{
    TK_REQUIRE(TK_CAP_NET);
    g_mysql = mysql_init(NULL);
    if (!g_mysql) return -1;

    char host[256], user[256], pass[256], db[256];
    int port = 3306;
    parse_mysql_dsn(dsn, host, sizeof(host), &port,
                    user, sizeof(user), pass, sizeof(pass),
                    db, sizeof(db));

    /* 121.21 (DAT-06): TLS is required.  No SSL option was set, so the
     * client used TLS only when the server offered it and otherwise sent the
     * DSN's credentials and every query in cleartext, with nothing to tell
     * the operator.  A server without TLS is now refused.  (REQUIRED
     * encrypts; it does not verify the server certificate -- the DSN has no
     * CA setting yet.) */
#if defined(MARIADB_BASE_VERSION) || defined(MARIADB_PACKAGE_VERSION_ID)
    my_bool enforce = 1;
    int opt_rc = mysql_options(g_mysql, MYSQL_OPT_SSL_ENFORCE, &enforce);
#else
    unsigned int ssl_mode = SSL_MODE_REQUIRED;
    int opt_rc = mysql_options(g_mysql, MYSQL_OPT_SSL_MODE, &ssl_mode);
#endif
    MYSQL *ok = opt_rc == 0 ? mysql_real_connect(g_mysql,
            host[0] ? host : "localhost",
            user[0] ? user : NULL,
            pass[0] ? pass : NULL,
            db[0] ? db : NULL,
            (unsigned)port, NULL, 0) : NULL;
    tk_secure_zero(pass, sizeof pass);
    if (!ok) {
        mysql_close(g_mysql); g_mysql = NULL;
        return -1;
    }
    return 0;
}

static void my_close(void)
{
    if (g_mysql) { mysql_close(g_mysql); g_mysql = NULL; }
}

static Row my_collect_row(MYSQL_RES *res, MYSQL_ROW mysql_row)
{
    unsigned int n = mysql_num_fields(res);
    MYSQL_FIELD *fields = mysql_fetch_fields(res);
    unsigned long *lengths = mysql_fetch_lengths(res);

    const char **names  = malloc(n * sizeof(char *));
    const char **values = malloc(n * sizeof(char *));
    int         *nulls  = malloc(n * sizeof(int));

    for (unsigned int c = 0; c < n; c++) {
        names[c]  = strdup(fields[c].name);
        nulls[c]  = (mysql_row[c] == NULL) ? 1 : 0;
        if (mysql_row[c])
            values[c] = strndup(mysql_row[c], lengths[c]);
        else
            values[c] = strdup("");
    }

    Row row;
    row.col_names  = names;
    row.col_values = values;
    row.col_nulls  = nulls;
    row.col_count  = (uint64_t)n;
    return row;
}

/* Internal struct for MySQL prepared statements, cast to/from TkStmt*. */
typedef struct {
    MYSQL_STMT   *stmt;
    MYSQL_RES    *meta;       /* result metadata (column info)            */
    unsigned int  col_count;  /* number of result columns                 */
    MYSQL_BIND   *result_binds; /* bind structs for fetching results      */
    char        **result_bufs;  /* buffers for column values              */
    unsigned long *result_lens; /* actual lengths returned by fetch       */
    my_bool      *result_nulls; /* null indicators for each column        */
} MyStmt;

/* 121.21 (DAT-03): a call with bind parameters goes through the prepared-
 * statement path (my_prepare / my_bind / my_step below), so each value
 * reaches the server out of band.  my_one / my_many / my_exec used to
 * `(void)params` and send the SQL text alone through mysql_query(): the `?`s
 * were never filled, and the parameterisation db.one / db.many / db.exec
 * promise (ADR-0011) did not exist on MySQL.  A call with no parameters
 * still uses mysql_query, so multi-statement text keeps working. */
static StmtResult my_prepare(int conn_id, const char *sql);
static BoolResult my_bind(TkStmt *stmt, StrArray params);
static RowResult  my_step(TkStmt *stmt);
static void       my_finalize(TkStmt *stmt);

static void my_free_row(Row *row)
{
    for (uint64_t c = 0; c < row->col_count; c++) {
        if (row->col_names)  free((void *)row->col_names[c]);
        if (row->col_values) free((void *)row->col_values[c]);
    }
    free((void *)row->col_names);
    free((void *)row->col_values);
    free(row->col_nulls);
}

/* Prepare `sql`, bind `params` and execute.  NULL and *err set on failure. */
static TkStmt *my_run_prepared(const char *sql, StrArray params, DbErr *err)
{
    StmtResult sr = my_prepare(0, sql);
    if (sr.is_err) { *err = sr.err; return NULL; }
    BoolResult br = my_bind(sr.ok, params);
    if (br.is_err) { *err = br.err; my_finalize(sr.ok); return NULL; }
    return sr.ok;
}

static RowResult my_one(const char *sql, StrArray params)
{
    RowResult r; r.is_err = 0;
    if (!g_mysql) { r.is_err = 1; r.err = my_err(DB_ERR_CONNECTION, "no connection"); return r; }

    if (params.len > 0) {
        TkStmt *st = my_run_prepared(sql, params, &r.err);
        if (!st) { r.is_err = 1; return r; }
        r = my_step(st);
        my_finalize(st);
        if (r.is_err && r.err.kind == DB_ERR_NOT_FOUND)
            r.err = my_err(DB_ERR_NOT_FOUND, "no rows returned");
        return r;
    }

    if (mysql_query(g_mysql, sql) != 0) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
        return r;
    }

    MYSQL_RES *res = mysql_store_result(g_mysql);
    if (!res) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
        return r;
    }

    MYSQL_ROW row = mysql_fetch_row(res);
    if (!row) {
        r.is_err = 1;
        r.err = my_err(DB_ERR_NOT_FOUND, "no rows returned");
        mysql_free_result(res);
        return r;
    }

    r.ok = my_collect_row(res, row);
    mysql_free_result(res);
    return r;
}

static RowArrayResult my_many(const char *sql, StrArray params)
{
    RowArrayResult r; r.is_err = 0;
    if (!g_mysql) { r.is_err = 1; r.err = my_err(DB_ERR_CONNECTION, "no connection"); return r; }

    if (params.len > 0) {
        TkStmt *st = my_run_prepared(sql, params, &r.err);
        if (!st) { r.is_err = 1; return r; }
        uint64_t cap = 8, n = 0;
        Row *rows = malloc(cap * sizeof(Row));
        if (!rows) { my_finalize(st); r.is_err = 1; r.err = my_err(DB_ERR_QUERY, "out of memory"); return r; }
        for (;;) {
            RowResult one = my_step(st);
            if (one.is_err) {
                if (one.err.kind == DB_ERR_NOT_FOUND) break;   /* done */
                for (uint64_t i = 0; i < n; i++) my_free_row(&rows[i]);
                free(rows); my_finalize(st);
                r.is_err = 1; r.err = one.err; return r;
            }
            if (n == cap) {
                Row *nr = realloc(rows, cap * 2 * sizeof(Row));
                if (!nr) {
                    my_free_row(&one.ok);
                    for (uint64_t i = 0; i < n; i++) my_free_row(&rows[i]);
                    free(rows); my_finalize(st);
                    r.is_err = 1; r.err = my_err(DB_ERR_QUERY, "out of memory"); return r;
                }
                rows = nr; cap *= 2;
            }
            rows[n++] = one.ok;
        }
        my_finalize(st);
        r.ok.data = rows; r.ok.len = n;
        return r;
    }

    if (mysql_query(g_mysql, sql) != 0) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
        return r;
    }

    MYSQL_RES *res = mysql_store_result(g_mysql);
    if (!res) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
        return r;
    }

    uint64_t nrows = mysql_num_rows(res);
    Row *rows = malloc((size_t)(nrows ? nrows : 1) * sizeof(Row));
    if (!rows) {   /* 121.21: was unchecked */
        mysql_free_result(res);
        r.is_err = 1; r.err = my_err(DB_ERR_QUERY, "out of memory"); return r;
    }
    uint64_t i = 0;
    MYSQL_ROW row;
    while (i < nrows && (row = mysql_fetch_row(res)) != NULL)
        rows[i++] = my_collect_row(res, row);

    r.ok.data = rows;
    r.ok.len  = i;
    mysql_free_result(res);
    return r;
}

static U64Result my_exec(const char *sql, StrArray params)
{
    U64Result r; r.is_err = 0; r.ok = 0;
    if (!g_mysql) { r.is_err = 1; r.err = my_err(DB_ERR_CONNECTION, "no connection"); return r; }

    if (params.len > 0) {
        TkStmt *st = my_run_prepared(sql, params, &r.err);
        if (!st) { r.is_err = 1; return r; }
        r.ok = (uint64_t)mysql_stmt_affected_rows(((MyStmt *)st)->stmt);
        my_finalize(st);
        return r;
    }

    if (mysql_query(g_mysql, sql) != 0) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
        return r;
    }
    r.ok = (uint64_t)mysql_affected_rows(g_mysql);
    return r;
}

static BoolResult my_begin(int conn_id)
{
    (void)conn_id;
    BoolResult r; r.is_err = 0; r.ok = 1;
    if (mysql_query(g_mysql, "START TRANSACTION") != 0) {
        r.is_err = 1; r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
    }
    return r;
}

static BoolResult my_commit(int conn_id)
{
    (void)conn_id;
    BoolResult r; r.is_err = 0; r.ok = 1;
    if (mysql_commit(g_mysql) != 0) {
        r.is_err = 1; r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
    }
    return r;
}

static BoolResult my_rollback(int conn_id)
{
    (void)conn_id;
    BoolResult r; r.is_err = 0; r.ok = 1;
    if (mysql_rollback(g_mysql) != 0) {
        r.is_err = 1; r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
    }
    return r;
}

static U64Result my_last_insert_id(int conn_id)
{
    (void)conn_id;
    U64Result r; r.is_err = 0;
    r.ok = (uint64_t)mysql_insert_id(g_mysql);
    return r;
}

static U64Result my_affected_rows(int conn_id)
{
    (void)conn_id;
    U64Result r; r.is_err = 0;
    r.ok = (uint64_t)mysql_affected_rows(g_mysql);
    return r;
}

/* 121.21 (DAT-04): the name is a bound parameter, as in the SQLite and
 * Postgres backends.  It was formatted into the SQL between quotes with no
 * escaping, so `x' OR '1'='1` answered "exists" for any name. */
static int my_table_exists(int conn_id, const char *name)
{
    (void)conn_id;
    if (!g_mysql || !name) return 0;
    const char *one[1] = { name };
    StrArray params = { one, 1 };
    RowResult r = my_one("SELECT 1 FROM information_schema.tables "
                         "WHERE table_schema=DATABASE() AND table_name=? LIMIT 1", params);
    if (r.is_err) return 0;
    my_free_row(&r.ok);
    return 1;
}

static StmtResult my_prepare(int conn_id, const char *sql)
{
    (void)conn_id;
    StmtResult r; r.is_err = 0; r.ok = NULL;

    if (!g_mysql) {
        r.is_err = 1;
        r.err = my_err(DB_ERR_CONNECTION, "no connection");
        return r;
    }

    MYSQL_STMT *raw = mysql_stmt_init(g_mysql);
    if (!raw) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_error(g_mysql));
        return r;
    }

    if (mysql_stmt_prepare(raw, sql, (unsigned long)strlen(sql)) != 0) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_stmt_error(raw));
        mysql_stmt_close(raw);
        return r;
    }

    MyStmt *s = calloc(1, sizeof(MyStmt));
    if (!s) {   /* 121.21: was unchecked */
        mysql_stmt_close(raw);
        r.is_err = 1; r.err = my_err(DB_ERR_QUERY, "out of memory");
        return r;
    }
    s->stmt = raw;

    /* Prepare result binding metadata if this is a SELECT-style query. */
    s->meta = mysql_stmt_result_metadata(raw);
    if (s->meta) {
        s->col_count    = mysql_num_fields(s->meta);
        s->result_binds = calloc(s->col_count, sizeof(MYSQL_BIND));
        s->result_bufs  = calloc(s->col_count, sizeof(char *));
        s->result_lens  = calloc(s->col_count, sizeof(unsigned long));
        s->result_nulls = calloc(s->col_count, sizeof(my_bool));

        MYSQL_FIELD *fields = mysql_fetch_fields(s->meta);
        for (unsigned int i = 0; i < s->col_count; i++) {
            unsigned long buflen = fields[i].length + 1;
            if (buflen < 256) buflen = 256;
            s->result_bufs[i] = calloc(1, buflen);
            s->result_binds[i].buffer_type   = MYSQL_TYPE_STRING;
            s->result_binds[i].buffer        = s->result_bufs[i];
            s->result_binds[i].buffer_length = buflen;
            s->result_binds[i].length        = &s->result_lens[i];
            s->result_binds[i].is_null       = &s->result_nulls[i];
        }
        mysql_stmt_bind_result(raw, s->result_binds);
    }

    r.ok = (TkStmt *)s;
    return r;
}

/* Bind `params` to the statement's placeholders and execute it.
 * 121.21: the placeholder count must equal the parameter count (binding
 * fewer makes libmysql read past the array); a failed bind is returned
 * instead of executing anyway; and a statement with no parameters is
 * executed too -- it used to return before mysql_stmt_execute, so a
 * prepared statement without `?`s never ran. */
static BoolResult my_bind(TkStmt *stmt, StrArray params)
{
    BoolResult r; r.is_err = 0; r.ok = 1;
    if (!stmt) {
        r.is_err = 1;
        r.err = my_err(DB_ERR_QUERY, "null statement");
        return r;
    }
    MyStmt *s = (MyStmt *)stmt;

    if ((uint64_t)mysql_stmt_param_count(s->stmt) != params.len) {
        r.is_err = 1;
        r.err = my_err(DB_ERR_QUERY, "parameter count does not match the statement's placeholders");
        return r;
    }

    MYSQL_BIND *binds = NULL;
    unsigned long *lengths = NULL;
    if (params.len > 0) {
        binds = calloc(params.len, sizeof(MYSQL_BIND));
        lengths = calloc(params.len, sizeof(unsigned long));
        if (!binds || !lengths) {
            free(binds); free(lengths);
            r.is_err = 1; r.err = my_err(DB_ERR_QUERY, "out of memory");
            return r;
        }
        for (uint64_t i = 0; i < params.len; i++) {
            lengths[i] = (unsigned long)strlen(params.data[i]);
            binds[i].buffer_type   = MYSQL_TYPE_STRING;
            binds[i].buffer        = (void *)params.data[i];
            binds[i].buffer_length = lengths[i];
            binds[i].length        = &lengths[i];
            binds[i].is_null       = NULL;
        }
        if (mysql_stmt_bind_param(s->stmt, binds) != 0) {
            r.is_err = 1;
            r.err = my_err_copy(DB_ERR_QUERY, mysql_stmt_error(s->stmt));
            free(lengths); free(binds);
            return r;
        }
    }

    if (mysql_stmt_execute(s->stmt) != 0) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_stmt_error(s->stmt));
    }

    free(lengths);
    free(binds);
    return r;
}

static RowResult my_step(TkStmt *stmt)
{
    RowResult r; r.is_err = 0;
    if (!stmt) {
        r.is_err = 1;
        r.err = my_err(DB_ERR_QUERY, "null statement");
        return r;
    }
    MyStmt *s = (MyStmt *)stmt;

    if (!s->meta) {
        /* Not a SELECT; no rows to fetch. */
        r.is_err = 1;
        r.err = my_err(DB_ERR_NOT_FOUND, "done");
        return r;
    }

    int rc = mysql_stmt_fetch(s->stmt);
    if (rc == MYSQL_NO_DATA) {
        r.is_err = 1;
        r.err = my_err(DB_ERR_NOT_FOUND, "done");
        return r;
    }
    if (rc != 0 && rc != MYSQL_DATA_TRUNCATED) {
        r.is_err = 1;
        r.err = my_err_copy(DB_ERR_QUERY, mysql_stmt_error(s->stmt));
        return r;
    }

    unsigned int n = s->col_count;
    MYSQL_FIELD *fields = mysql_fetch_fields(s->meta);
    const char **names  = malloc(n * sizeof(char *));
    const char **values = malloc(n * sizeof(char *));
    int         *nulls  = malloc(n * sizeof(int));

    for (unsigned int c = 0; c < n; c++) {
        names[c] = strdup(fields[c].name);
        nulls[c] = s->result_nulls[c] ? 1 : 0;
        if (s->result_nulls[c])
            values[c] = strdup("");
        else
            values[c] = strndup(s->result_bufs[c], s->result_lens[c]);
    }

    r.ok.col_names  = names;
    r.ok.col_values = values;
    r.ok.col_nulls  = nulls;
    r.ok.col_count  = (uint64_t)n;
    return r;
}

static void my_finalize(TkStmt *stmt)
{
    if (!stmt) return;
    MyStmt *s = (MyStmt *)stmt;
    if (s->meta) {
        for (unsigned int i = 0; i < s->col_count; i++)
            free(s->result_bufs[i]);
        free(s->result_bufs);
        free(s->result_binds);
        free(s->result_lens);
        free(s->result_nulls);
        mysql_free_result(s->meta);
    }
    mysql_stmt_close(s->stmt);
    free(s);
}

const DbBackend db_mysql_backend = {
    .name            = "mysql",
    .open            = my_open,
    .close           = my_close,
    .one             = my_one,
    .many            = my_many,
    .exec            = my_exec,
    .prepare         = my_prepare,
    .bind            = my_bind,
    .step            = my_step,
    .finalize        = my_finalize,
    .begin           = my_begin,
    .commit          = my_commit,
    .rollback        = my_rollback,
    .last_insert_id  = my_last_insert_id,
    .affected_rows   = my_affected_rows,
    .table_exists    = my_table_exists,
};

#else /* !TK_HAVE_MYSQL */

/* 135.3d: without the backend macro this file would be an EMPTY translation
 * unit, which ISO C forbids (-Wpedantic) and which is indistinguishable from a
 * file whose whole body was #ifdef-ed out by mistake.  Say so in code. */
typedef int tk_db_mysql_unused;

#endif /* TK_HAVE_MYSQL */
