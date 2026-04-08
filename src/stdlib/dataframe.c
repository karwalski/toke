/*
 * dataframe.c — Implementation of the std.dataframe standard library module.
 *
 * Provides an in-memory columnar dataframe backed by RFC 4180 CSV ingestion.
 * Each column is either DF_COL_F64 (all values parseable as double) or
 * DF_COL_STR (anything else).
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code. Callers own the returned pointers; use df_free().
 *
 * No external dependencies beyond libc and csv.h.
 *
 * Story: 16.1.3
 */

#include "dataframe.h"
#include "csv.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

/* =========================================================================
 * Internal helpers
 * ========================================================================= */

/* Duplicate a C string onto the heap.  Returns NULL if s is NULL. */
static char *df_strdup(const char *s)
{
    if (!s) return NULL;
    size_t n = strlen(s) + 1;
    char *p = malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* Try to parse s as a finite double.  Returns 1 on success, 0 otherwise.
 * Empty strings are NOT considered numeric. */
static int is_f64(const char *s)
{
    if (!s || *s == '\0') return 0;
    char *end;
    errno = 0;
    strtod(s, &end);
    if (errno != 0) return 0;
    /* entire string must be consumed */
    while (*end == ' ' || *end == '\t') end++;
    return *end == '\0';
}

/* Allocate a new, empty DfCol with the given name and type.
 * f64_data / str_data are set to NULL; nrows is 0. */
static DfCol make_col(const char *name, DfColType type)
{
    DfCol c;
    c.name     = df_strdup(name);
    c.type     = type;
    c.f64_data = NULL;
    c.str_data = NULL;
    c.nrows    = 0;
    return c;
}

/* Free the contents of a single DfCol (but not the DfCol struct itself). */
static void col_free_contents(DfCol *c)
{
    if (!c) return;
    free((char *)c->name);
    if (c->type == DF_COL_F64) {
        free(c->f64_data);
    } else {
        if (c->str_data) {
            for (uint64_t i = 0; i < c->nrows; i++)
                free(c->str_data[i]);
            free(c->str_data);
        }
    }
}

/* =========================================================================
 * df_new
 * ========================================================================= */

TkDataframe *df_new(void)
{
    TkDataframe *df = calloc(1, sizeof(TkDataframe));
    return df;
}

/* =========================================================================
 * df_free
 * ========================================================================= */

void df_free(TkDataframe *df)
{
    if (!df) return;
    for (uint64_t j = 0; j < df->ncols; j++)
        col_free_contents(&df->cols[j]);
    free(df->cols);
    free(df);
}

/* =========================================================================
 * df_fromcsv
 * ========================================================================= */

DfResult df_fromcsv(const char *csv_data, uint64_t csv_len, int has_header)
{
    DfResult result = {NULL, 0, NULL};

    if (!csv_data || csv_len == 0) {
        result.is_err = 1;
        result.err_msg = "df_fromcsv: empty input";
        return result;
    }

    uint64_t nrows_raw = 0;
    StrArray *rows = csv_parse(csv_data, csv_len, &nrows_raw);
    if (!rows || nrows_raw == 0) {
        result.is_err = 1;
        result.err_msg = "df_fromcsv: csv_parse returned no rows";
        return result;
    }

    /* Determine header row and data row range. */
    uint64_t header_idx  = 0;   /* index of the header row in rows[] */
    uint64_t data_start  = has_header ? 1 : 0;
    uint64_t data_nrows  = (nrows_raw > data_start) ? nrows_raw - data_start : 0;
    uint64_t ncols       = rows[header_idx].len;

    if (ncols == 0) {
        result.is_err = 1;
        result.err_msg = "df_fromcsv: no columns found";
        /* free rows */
        for (uint64_t r = 0; r < nrows_raw; r++) {
            for (uint64_t c = 0; c < rows[r].len; c++)
                free((char *)rows[r].data[c]);
            free((char **)rows[r].data);
        }
        free(rows);
        return result;
    }

    /* Build column names. */
    char **col_names = malloc(ncols * sizeof(char *));
    if (!col_names) goto oom;

    if (has_header) {
        for (uint64_t j = 0; j < ncols; j++)
            col_names[j] = df_strdup(rows[0].data[j]);
    } else {
        char buf[32];
        for (uint64_t j = 0; j < ncols; j++) {
            snprintf(buf, sizeof(buf), "col%llu", (unsigned long long)j);
            col_names[j] = df_strdup(buf);
        }
    }

    /* Detect column types: if every data value in the column is f64 → F64. */
    DfColType *types = malloc(ncols * sizeof(DfColType));
    if (!types) goto oom;

    for (uint64_t j = 0; j < ncols; j++) {
        types[j] = DF_COL_F64;  /* optimistic */
        for (uint64_t r = 0; r < data_nrows; r++) {
            uint64_t row_idx = data_start + r;
            const char *val = (j < rows[row_idx].len) ? rows[row_idx].data[j] : "";
            if (!is_f64(val)) {
                types[j] = DF_COL_STR;
                break;
            }
        }
    }

    /* Allocate the dataframe and columns. */
    TkDataframe *df = df_new();
    if (!df) goto oom;
    df->ncols = ncols;
    df->nrows = data_nrows;
    df->cols  = calloc(ncols, sizeof(DfCol));
    if (!df->cols) { free(df); goto oom; }

    for (uint64_t j = 0; j < ncols; j++) {
        df->cols[j] = make_col(col_names[j], types[j]);
        df->cols[j].nrows = data_nrows;

        if (types[j] == DF_COL_F64) {
            df->cols[j].f64_data = malloc(data_nrows * sizeof(double));
            if (!df->cols[j].f64_data) { df_free(df); goto oom; }
            for (uint64_t r = 0; r < data_nrows; r++) {
                uint64_t row_idx = data_start + r;
                const char *val = (j < rows[row_idx].len) ? rows[row_idx].data[j] : "0";
                df->cols[j].f64_data[r] = strtod(val, NULL);
            }
        } else {
            df->cols[j].str_data = malloc(data_nrows * sizeof(char *));
            if (!df->cols[j].str_data) { df_free(df); goto oom; }
            for (uint64_t r = 0; r < data_nrows; r++) {
                uint64_t row_idx = data_start + r;
                const char *val = (j < rows[row_idx].len) ? rows[row_idx].data[j] : "";
                df->cols[j].str_data[r] = df_strdup(val);
            }
        }
    }

    /* Cleanup temporaries. */
    for (uint64_t j = 0; j < ncols; j++) free(col_names[j]);
    free(col_names);
    free(types);
    for (uint64_t r = 0; r < nrows_raw; r++) {
        for (uint64_t c = 0; c < rows[r].len; c++)
            free((char *)rows[r].data[c]);
        free((char **)rows[r].data);
    }
    free(rows);

    result.ok = df;
    return result;

oom:
    result.is_err = 1;
    result.err_msg = "df_fromcsv: out of memory";
    return result;
}

/* =========================================================================
 * df_column
 * ========================================================================= */

DfCol *df_column(TkDataframe *df, const char *name)
{
    if (!df || !name) return NULL;
    for (uint64_t j = 0; j < df->ncols; j++) {
        if (df->cols[j].name && strcmp(df->cols[j].name, name) == 0)
            return &df->cols[j];
    }
    return NULL;
}

/* =========================================================================
 * df_shape
 * ========================================================================= */

void df_shape(TkDataframe *df, uint64_t *nrows_out, uint64_t *ncols_out)
{
    if (!df) {
        if (nrows_out) *nrows_out = 0;
        if (ncols_out) *ncols_out = 0;
        return;
    }
    if (nrows_out) *nrows_out = df->nrows;
    if (ncols_out) *ncols_out = df->ncols;
}

/* =========================================================================
 * Internal: copy_rows_subset — build a new dataframe with a subset of rows.
 * row_mask[r] != 0  →  row r is included.
 * ========================================================================= */

static TkDataframe *copy_rows_subset(TkDataframe *src, const int *row_mask,
                                     uint64_t new_nrows)
{
    TkDataframe *dst = df_new();
    if (!dst) return NULL;
    dst->ncols = src->ncols;
    dst->nrows = new_nrows;
    dst->cols  = calloc(src->ncols, sizeof(DfCol));
    if (!dst->cols) { free(dst); return NULL; }

    for (uint64_t j = 0; j < src->ncols; j++) {
        DfCol *sc = &src->cols[j];
        DfCol *dc = &dst->cols[j];
        dc->name  = df_strdup(sc->name);
        dc->type  = sc->type;
        dc->nrows = new_nrows;

        if (sc->type == DF_COL_F64) {
            dc->f64_data = malloc(new_nrows * sizeof(double));
            if (!dc->f64_data) { df_free(dst); return NULL; }
            uint64_t k = 0;
            for (uint64_t r = 0; r < src->nrows; r++)
                if (row_mask[r]) dc->f64_data[k++] = sc->f64_data[r];
        } else {
            dc->str_data = malloc(new_nrows * sizeof(char *));
            if (!dc->str_data) { df_free(dst); return NULL; }
            uint64_t k = 0;
            for (uint64_t r = 0; r < src->nrows; r++)
                if (row_mask[r]) dc->str_data[k++] = df_strdup(sc->str_data[r]);
        }
    }
    return dst;
}

/* =========================================================================
 * df_filter
 * ========================================================================= */

TkDataframe *df_filter(TkDataframe *df, const char *col,
                       double threshold, int op)
{
    if (!df || !col) return NULL;
    DfCol *c = df_column(df, col);
    if (!c || c->type != DF_COL_F64) return NULL;

    int *mask = calloc(df->nrows, sizeof(int));
    if (!mask) return NULL;

    uint64_t new_nrows = 0;
    for (uint64_t r = 0; r < df->nrows; r++) {
        double v = c->f64_data[r];
        int keep = 0;
        switch (op) {
            case 0: keep = (v <  threshold); break;
            case 1: keep = (v <= threshold); break;
            case 2: keep = (v == threshold); break;
            case 3: keep = (v >= threshold); break;
            case 4: keep = (v >  threshold); break;
            default: break;
        }
        mask[r] = keep;
        if (keep) new_nrows++;
    }

    TkDataframe *result = copy_rows_subset(df, mask, new_nrows);
    free(mask);
    return result;
}

/* =========================================================================
 * df_head
 * ========================================================================= */

TkDataframe *df_head(TkDataframe *df, uint64_t n)
{
    if (!df) return NULL;
    uint64_t take = (n < df->nrows) ? n : df->nrows;
    int *mask = calloc(df->nrows, sizeof(int));
    if (!mask) return NULL;
    for (uint64_t r = 0; r < take; r++) mask[r] = 1;
    TkDataframe *result = copy_rows_subset(df, mask, take);
    free(mask);
    return result;
}

/* =========================================================================
 * df_tojson
 * ========================================================================= */

/* Simple dynamic string buffer for JSON construction. */
typedef struct {
    char    *buf;
    size_t   len;
    size_t   cap;
} DfBuf;

static int sb_init(DfBuf *sb)
{
    sb->cap = 256;
    sb->len = 0;
    sb->buf = malloc(sb->cap);
    return sb->buf ? 0 : -1;
}

static int sb_grow(DfBuf *sb, size_t needed)
{
    while (sb->len + needed + 1 > sb->cap) {
        sb->cap *= 2;
        char *p = realloc(sb->buf, sb->cap);
        if (!p) return -1;
        sb->buf = p;
    }
    return 0;
}

static void sb_append(DfBuf *sb, const char *s)
{
    size_t n = strlen(s);
    if (sb_grow(sb, n) == 0) {
        memcpy(sb->buf + sb->len, s, n);
        sb->len += n;
        sb->buf[sb->len] = '\0';
    }
}

static void sb_append_f64(DfBuf *sb, double v)
{
    char tmp[64];
    /* Use %g to produce compact output; fall back to full precision. */
    snprintf(tmp, sizeof(tmp), "%.17g", v);
    sb_append(sb, tmp);
}

/* Append a JSON-escaped quoted string. */
static void sb_append_json_str(DfBuf *sb, const char *s)
{
    sb_append(sb, "\"");
    for (; *s; s++) {
        char esc[4] = {0, 0, 0, 0};
        switch (*s) {
            case '"':  esc[0]='\\'; esc[1]='"';  break;
            case '\\': esc[0]='\\'; esc[1]='\\'; break;
            case '\n': esc[0]='\\'; esc[1]='n';  break;
            case '\r': esc[0]='\\'; esc[1]='r';  break;
            case '\t': esc[0]='\\'; esc[1]='t';  break;
            default:   esc[0]=*s; break;
        }
        sb_append(sb, esc);
    }
    sb_append(sb, "\"");
}

const char *df_tojson(TkDataframe *df)
{
    if (!df) return NULL;

    DfBuf sb;
    if (sb_init(&sb) != 0) return NULL;

    sb_append(&sb, "[");
    for (uint64_t r = 0; r < df->nrows; r++) {
        if (r > 0) sb_append(&sb, ",");
        sb_append(&sb, "{");
        for (uint64_t j = 0; j < df->ncols; j++) {
            if (j > 0) sb_append(&sb, ",");
            DfCol *c = &df->cols[j];
            sb_append_json_str(&sb, c->name);
            sb_append(&sb, ":");
            if (c->type == DF_COL_F64) {
                sb_append_f64(&sb, c->f64_data[r]);
            } else {
                const char *sv = c->str_data ? c->str_data[r] : "";
                sb_append_json_str(&sb, sv ? sv : "");
            }
        }
        sb_append(&sb, "}");
    }
    sb_append(&sb, "]");

    return sb.buf;  /* caller owns */
}

/* =========================================================================
 * df_join  (inner hash join on a string column)
 * ========================================================================= */

/* A very simple open-addressing hash table mapping string → list of row indices. */

#define HT_INIT_SIZE 64

typedef struct HtNode {
    char        *key;
    uint64_t    *rows;       /* row indices in left frame */
    uint64_t     nrows;
    uint64_t     cap;
    struct HtNode *next;     /* chaining */
} HtNode;

typedef struct {
    HtNode  **buckets;
    uint64_t  size;
} HashTable;

static uint64_t ht_hash(const char *s, uint64_t size)
{
    uint64_t h = 14695981039346656037ULL;
    for (; *s; s++) {
        h ^= (unsigned char)*s;
        h *= 1099511628211ULL;
    }
    return h % size;
}

static HashTable *ht_new(uint64_t size)
{
    HashTable *ht = calloc(1, sizeof(HashTable));
    if (!ht) return NULL;
    ht->size    = size;
    ht->buckets = calloc(size, sizeof(HtNode *));
    if (!ht->buckets) { free(ht); return NULL; }
    return ht;
}

static void ht_insert(HashTable *ht, const char *key, uint64_t row_idx)
{
    uint64_t h = ht_hash(key, ht->size);
    HtNode *n = ht->buckets[h];
    while (n) {
        if (strcmp(n->key, key) == 0) {
            /* append row index */
            if (n->nrows == n->cap) {
                n->cap = n->cap ? n->cap * 2 : 4;
                n->rows = realloc(n->rows, n->cap * sizeof(uint64_t));
            }
            n->rows[n->nrows++] = row_idx;
            return;
        }
        n = n->next;
    }
    /* new node */
    n = calloc(1, sizeof(HtNode));
    n->key      = df_strdup(key);
    n->cap      = 4;
    n->rows     = malloc(4 * sizeof(uint64_t));
    n->rows[0]  = row_idx;
    n->nrows    = 1;
    n->next     = ht->buckets[h];
    ht->buckets[h] = n;
}

static HtNode *ht_lookup(HashTable *ht, const char *key)
{
    uint64_t h = ht_hash(key, ht->size);
    HtNode *n = ht->buckets[h];
    while (n) {
        if (strcmp(n->key, key) == 0) return n;
        n = n->next;
    }
    return NULL;
}

static void ht_free(HashTable *ht)
{
    if (!ht) return;
    for (uint64_t i = 0; i < ht->size; i++) {
        HtNode *n = ht->buckets[i];
        while (n) {
            HtNode *nx = n->next;
            free(n->key);
            free(n->rows);
            free(n);
            n = nx;
        }
    }
    free(ht->buckets);
    free(ht);
}

DfResult df_join(TkDataframe *left, TkDataframe *right, const char *on_col)
{
    DfResult res = {NULL, 0, NULL};

    if (!left || !right || !on_col) {
        res.is_err = 1; res.err_msg = "df_join: NULL argument"; return res;
    }

    DfCol *lcol = df_column(left, on_col);
    DfCol *rcol = df_column(right, on_col);

    if (!lcol || lcol->type != DF_COL_STR) {
        res.is_err = 1; res.err_msg = "df_join: key column not found in left or not DF_COL_STR";
        return res;
    }
    if (!rcol || rcol->type != DF_COL_STR) {
        res.is_err = 1; res.err_msg = "df_join: key column not found in right or not DF_COL_STR";
        return res;
    }

    /* Build hash table on left key column. */
    uint64_t ht_size = left->nrows * 2 + HT_INIT_SIZE;
    HashTable *ht = ht_new(ht_size);
    if (!ht) { res.is_err = 1; res.err_msg = "df_join: OOM"; return res; }

    for (uint64_t r = 0; r < left->nrows; r++)
        ht_insert(ht, lcol->str_data[r], r);

    /* First pass: count output rows. */
    uint64_t out_nrows = 0;
    for (uint64_t r = 0; r < right->nrows; r++) {
        HtNode *nd = ht_lookup(ht, rcol->str_data[r]);
        if (nd) out_nrows += nd->nrows;
    }

    /* Build output schema:
     *   all left cols, then all right cols except the join key. */
    uint64_t right_extra = 0;
    for (uint64_t j = 0; j < right->ncols; j++)
        if (strcmp(right->cols[j].name, on_col) != 0) right_extra++;

    uint64_t out_ncols = left->ncols + right_extra;

    TkDataframe *out = df_new();
    if (!out) { ht_free(ht); res.is_err = 1; res.err_msg = "df_join: OOM"; return res; }
    out->ncols = out_ncols;
    out->nrows = out_nrows;
    out->cols  = calloc(out_ncols, sizeof(DfCol));
    if (!out->cols) { df_free(out); ht_free(ht); res.is_err = 1; res.err_msg = "df_join: OOM"; return res; }

    /* Allocate output column storage. */
    for (uint64_t j = 0; j < left->ncols; j++) {
        out->cols[j].name  = df_strdup(left->cols[j].name);
        out->cols[j].type  = left->cols[j].type;
        out->cols[j].nrows = out_nrows;
        if (left->cols[j].type == DF_COL_F64)
            out->cols[j].f64_data = malloc(out_nrows * sizeof(double));
        else
            out->cols[j].str_data = malloc(out_nrows * sizeof(char *));
    }
    uint64_t oj = left->ncols;
    for (uint64_t j = 0; j < right->ncols; j++) {
        if (strcmp(right->cols[j].name, on_col) == 0) continue;
        out->cols[oj].name  = df_strdup(right->cols[j].name);
        out->cols[oj].type  = right->cols[j].type;
        out->cols[oj].nrows = out_nrows;
        if (right->cols[j].type == DF_COL_F64)
            out->cols[oj].f64_data = malloc(out_nrows * sizeof(double));
        else
            out->cols[oj].str_data = malloc(out_nrows * sizeof(char *));
        oj++;
    }

    /* Second pass: fill output rows. */
    uint64_t out_r = 0;
    for (uint64_t rr = 0; rr < right->nrows; rr++) {
        HtNode *nd = ht_lookup(ht, rcol->str_data[rr]);
        if (!nd) continue;
        for (uint64_t m = 0; m < nd->nrows; m++) {
            uint64_t lr = nd->rows[m];
            /* Copy left columns. */
            for (uint64_t j = 0; j < left->ncols; j++) {
                DfCol *src = &left->cols[j];
                DfCol *dst = &out->cols[j];
                if (src->type == DF_COL_F64)
                    dst->f64_data[out_r] = src->f64_data[lr];
                else
                    dst->str_data[out_r] = df_strdup(src->str_data[lr]);
            }
            /* Copy right non-key columns. */
            uint64_t oj2 = left->ncols;
            for (uint64_t j = 0; j < right->ncols; j++) {
                if (strcmp(right->cols[j].name, on_col) == 0) continue;
                DfCol *src = &right->cols[j];
                DfCol *dst = &out->cols[oj2];
                if (src->type == DF_COL_F64)
                    dst->f64_data[out_r] = src->f64_data[rr];
                else
                    dst->str_data[out_r] = df_strdup(src->str_data[rr]);
                oj2++;
            }
            out_r++;
        }
    }

    ht_free(ht);
    res.ok = out;
    return res;
}

/* =========================================================================
 * df_groupby
 * ========================================================================= */

DfGroupResult df_groupby(TkDataframe *df, const char *group_col,
                         const char *agg_col, int agg)
{
    DfGroupResult empty = {NULL, 0};
    if (!df || !group_col || !agg_col) return empty;

    DfCol *gc = df_column(df, group_col);
    DfCol *ac = df_column(df, agg_col);
    if (!gc || gc->type != DF_COL_STR) return empty;
    if (!ac || ac->type != DF_COL_F64) return empty;

    /* Use a hash table to track unique group values and their accumulators. */
    uint64_t ht_size = df->nrows * 2 + HT_INIT_SIZE;

    /* We'll build a separate lightweight accumulator structure. */
    typedef struct GroupAcc {
        char           *key;
        double          sum;
        uint64_t        count;
        struct GroupAcc *next;
    } GroupAcc;

    uint64_t bkt_size = ht_size;
    GroupAcc **buckets = calloc(bkt_size, sizeof(GroupAcc *));
    if (!buckets) return empty;
    uint64_t n_groups = 0;

    for (uint64_t r = 0; r < df->nrows; r++) {
        const char *key = gc->str_data[r];
        double val      = ac->f64_data[r];
        uint64_t h      = ht_hash(key, bkt_size);
        GroupAcc *ga    = buckets[h];
        while (ga) {
            if (strcmp(ga->key, key) == 0) break;
            ga = ga->next;
        }
        if (!ga) {
            ga = calloc(1, sizeof(GroupAcc));
            if (!ga) {
                /* leak and return empty on OOM */
                free(buckets);
                return empty;
            }
            ga->key    = df_strdup(key);
            ga->next   = buckets[h];
            buckets[h] = ga;
            n_groups++;
        }
        ga->sum   += val;
        ga->count += 1;
    }

    /* Collect results. */
    DfGroupRow *rows = malloc(n_groups * sizeof(DfGroupRow));
    if (!rows) {
        free(buckets);
        return empty;
    }

    uint64_t idx = 0;
    for (uint64_t i = 0; i < bkt_size && idx < n_groups; i++) {
        GroupAcc *ga = buckets[i];
        while (ga) {
            rows[idx].group_val = ga->key;  /* transfer ownership */
            switch (agg) {
                case 0: rows[idx].agg_result = (double)ga->count; break;
                case 1: rows[idx].agg_result = ga->sum;           break;
                case 2: rows[idx].agg_result = ga->count > 0
                            ? ga->sum / (double)ga->count : 0.0;  break;
                default: rows[idx].agg_result = 0.0; break;
            }
            idx++;
            GroupAcc *nx = ga->next;
            free(ga);   /* key was transferred; do not free here */
            ga = nx;
        }
    }
    free(buckets);

    DfGroupResult gr;
    gr.rows  = rows;
    gr.nrows = n_groups;
    return gr;
}

/* =========================================================================
 * df_fromrows  (build dataframe from row-major string data)
 * ========================================================================= */

TkDataframe *df_fromrows(const char **headers, uint64_t ncols,
                          const char ***rows, uint64_t nrows)
{
    if (!headers || ncols == 0) return NULL;

    /* Detect column types: if every data value parses as f64 → DF_COL_F64. */
    DfColType *types = malloc(ncols * sizeof(DfColType));
    if (!types) return NULL;

    for (uint64_t j = 0; j < ncols; j++) {
        types[j] = DF_COL_F64; /* optimistic */
        for (uint64_t r = 0; r < nrows; r++) {
            const char *val = rows[r][j];
            if (!is_f64(val)) {
                types[j] = DF_COL_STR;
                break;
            }
        }
    }

    TkDataframe *df = df_new();
    if (!df) { free(types); return NULL; }
    df->ncols = ncols;
    df->nrows = nrows;
    df->cols  = calloc(ncols, sizeof(DfCol));
    if (!df->cols) { free(df); free(types); return NULL; }

    for (uint64_t j = 0; j < ncols; j++) {
        df->cols[j] = make_col(headers[j], types[j]);
        df->cols[j].nrows = nrows;

        if (types[j] == DF_COL_F64) {
            df->cols[j].f64_data = malloc(nrows * sizeof(double));
            if (!df->cols[j].f64_data) { df_free(df); free(types); return NULL; }
            for (uint64_t r = 0; r < nrows; r++) {
                const char *val = rows[r][j];
                df->cols[j].f64_data[r] = strtod(val ? val : "0", NULL);
            }
        } else {
            df->cols[j].str_data = malloc(nrows * sizeof(char *));
            if (!df->cols[j].str_data) { df_free(df); free(types); return NULL; }
            for (uint64_t r = 0; r < nrows; r++) {
                const char *val = rows[r][j];
                df->cols[j].str_data[r] = df_strdup(val ? val : "");
            }
        }
    }

    free(types);
    return df;
}

/* =========================================================================
 * df_columnstr  (extract a string column by name)
 * ========================================================================= */

char **df_columnstr(TkDataframe *df, const char *name, uint64_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!df || !name) return NULL;

    DfCol *c = df_column(df, name);
    if (!c || c->type != DF_COL_STR) return NULL;

    char **result = malloc(c->nrows * sizeof(char *));
    if (!result) return NULL;

    for (uint64_t r = 0; r < c->nrows; r++) {
        result[r] = df_strdup(c->str_data[r]);
        if (!result[r]) {
            /* clean up on OOM */
            for (uint64_t k = 0; k < r; k++) free(result[k]);
            free(result);
            return NULL;
        }
    }

    if (out_len) *out_len = c->nrows;
    return result;
}

/* =========================================================================
 * df_tocsv  (serialise dataframe as CSV string)
 * ========================================================================= */

const char *df_tocsv(TkDataframe *df)
{
    if (!df) return NULL;

    TkCsvWriter *w = csv_writer_new();
    if (!w) return NULL;

    /* Header row. */
    {
        const char **hdr = malloc(df->ncols * sizeof(const char *));
        if (!hdr) { csv_writer_free(w); return NULL; }
        for (uint64_t j = 0; j < df->ncols; j++)
            hdr[j] = df->cols[j].name;
        StrArray row;
        row.data = hdr;
        row.len  = df->ncols;
        csv_writer_writerow(w, row);
        free(hdr);
    }

    /* Data rows. */
    {
        const char **fields = malloc(df->ncols * sizeof(const char *));
        char       **owned  = malloc(df->ncols * sizeof(char *));
        if (!fields || !owned) {
            free(fields); free(owned);
            csv_writer_free(w);
            return NULL;
        }

        for (uint64_t r = 0; r < df->nrows; r++) {
            for (uint64_t j = 0; j < df->ncols; j++) {
                if (df->cols[j].type == DF_COL_F64) {
                    char buf[64];
                    snprintf(buf, sizeof(buf), "%.17g", df->cols[j].f64_data[r]);
                    owned[j] = df_strdup(buf);
                    fields[j] = owned[j];
                } else {
                    owned[j] = NULL;
                    fields[j] = df->cols[j].str_data[r]
                                    ? df->cols[j].str_data[r] : "";
                }
            }
            StrArray row;
            row.data = fields;
            row.len  = df->ncols;
            csv_writer_writerow(w, row);
            for (uint64_t j = 0; j < df->ncols; j++)
                free(owned[j]);
        }
        free(fields);
        free(owned);
    }

    const char *csv = csv_writer_flush(w);
    /* We need to own the string after freeing the writer, so dup it. */
    char *result = df_strdup(csv);
    csv_writer_free(w);
    return result;
}

/* =========================================================================
 * Story 31.1.1: df_sort, df_unique, df_drop_column, df_rename_column,
 *               df_select_columns, df_value_counts
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * df_sort
 * ------------------------------------------------------------------------- */

/* Comparison context passed through qsort via a global (C99 has no qsort_r
 * in all implementations, so we use a file-scope struct). */
typedef struct {
    DfCol *sort_col;
    int    ascending;
} SortCtx;

static SortCtx g_sort_ctx;

/* Get a string representation of row r from col, used for sort comparisons. */
static const char *sort_col_str(DfCol *c, uint64_t r)
{
    if (c->type == DF_COL_STR) {
        return c->str_data ? c->str_data[r] : "";
    }
    return NULL;  /* f64 columns are handled separately */
}

/* qsort comparator working on uint64_t row indices. */
static int sort_cmp_rows(const void *a, const void *b)
{
    uint64_t ra = *(const uint64_t *)a;
    uint64_t rb = *(const uint64_t *)b;
    DfCol  *c   = g_sort_ctx.sort_col;
    int     asc = g_sort_ctx.ascending;
    int     cmp = 0;

    if (c->type == DF_COL_F64) {
        double va = c->f64_data[ra];
        double vb = c->f64_data[rb];
        if (va < vb) cmp = -1;
        else if (va > vb) cmp = 1;
        else cmp = 0;
    } else {
        /* String column: try numeric parse first */
        const char *sa = sort_col_str(c, ra);
        const char *sb = sort_col_str(c, rb);
        if (!sa) sa = "";
        if (!sb) sb = "";
        char *ea, *eb;
        double na = strtod(sa, &ea);
        double nb = strtod(sb, &eb);
        /* Both fully numeric? compare as doubles */
        if (*ea == '\0' && *eb == '\0' && sa != ea && sb != eb) {
            if (na < nb) cmp = -1;
            else if (na > nb) cmp = 1;
            else cmp = 0;
        } else {
            cmp = strcmp(sa, sb);
        }
    }

    return asc ? cmp : -cmp;
}

TkDataframe *df_sort(TkDataframe *df, const char *col, int ascending)
{
    if (!df || !col) return NULL;
    DfCol *c = df_column(df, col);
    if (!c) return NULL;

    /* Build an array of row indices then sort it. */
    uint64_t *idx = malloc(df->nrows * sizeof(uint64_t));
    if (!idx) return NULL;
    for (uint64_t r = 0; r < df->nrows; r++) idx[r] = r;

    g_sort_ctx.sort_col  = c;
    g_sort_ctx.ascending = ascending;
    qsort(idx, (size_t)df->nrows, sizeof(uint64_t), sort_cmp_rows);

    /* Build the result dataframe in sorted order. */
    TkDataframe *dst = df_new();
    if (!dst) { free(idx); return NULL; }
    dst->ncols = df->ncols;
    dst->nrows = df->nrows;
    dst->cols  = calloc(df->ncols, sizeof(DfCol));
    if (!dst->cols) { free(idx); df_free(dst); return NULL; }

    for (uint64_t j = 0; j < df->ncols; j++) {
        DfCol *sc = &df->cols[j];
        DfCol *dc = &dst->cols[j];
        dc->name  = df_strdup(sc->name);
        dc->type  = sc->type;
        dc->nrows = df->nrows;

        if (sc->type == DF_COL_F64) {
            dc->f64_data = malloc(df->nrows * sizeof(double));
            if (!dc->f64_data) { free(idx); df_free(dst); return NULL; }
            for (uint64_t r = 0; r < df->nrows; r++)
                dc->f64_data[r] = sc->f64_data[idx[r]];
        } else {
            dc->str_data = malloc(df->nrows * sizeof(char *));
            if (!dc->str_data) { free(idx); df_free(dst); return NULL; }
            for (uint64_t r = 0; r < df->nrows; r++)
                dc->str_data[r] = df_strdup(sc->str_data[idx[r]]);
        }
    }

    free(idx);
    return dst;
}

/* -------------------------------------------------------------------------
 * df_unique
 * ------------------------------------------------------------------------- */

TkDataframe *df_unique(TkDataframe *df, const char *col)
{
    if (!df || !col) return NULL;
    DfCol *c = df_column(df, col);
    if (!c) return NULL;

    /* Build a mask: include row r if its value hasn't been seen before. */
    int *mask = calloc(df->nrows, sizeof(int));
    if (!mask) return NULL;

    /* Use the existing hash table infrastructure for dedup tracking. */
    uint64_t ht_size = df->nrows * 2 + HT_INIT_SIZE;
    HashTable *seen = ht_new(ht_size);
    if (!seen) { free(mask); return NULL; }

    uint64_t new_nrows = 0;
    for (uint64_t r = 0; r < df->nrows; r++) {
        char tmp[64];
        const char *key;
        if (c->type == DF_COL_F64) {
            snprintf(tmp, sizeof(tmp), "%.17g", c->f64_data[r]);
            key = tmp;
        } else {
            key = c->str_data ? (c->str_data[r] ? c->str_data[r] : "") : "";
        }
        if (!ht_lookup(seen, key)) {
            /* Mark as seen by inserting a dummy row index */
            ht_insert(seen, key, r);
            mask[r] = 1;
            new_nrows++;
        }
    }

    ht_free(seen);
    TkDataframe *result = copy_rows_subset(df, mask, new_nrows);
    free(mask);
    return result;
}

/* -------------------------------------------------------------------------
 * df_drop_column
 * ------------------------------------------------------------------------- */

TkDataframe *df_drop_column(TkDataframe *df, const char *col)
{
    if (!df || !col) return NULL;

    /* Find the column index to drop (or -1 if not found). */
    int64_t drop_idx = -1;
    for (uint64_t j = 0; j < df->ncols; j++) {
        if (df->cols[j].name && strcmp(df->cols[j].name, col) == 0) {
            drop_idx = (int64_t)j;
            break;
        }
    }

    uint64_t new_ncols = (drop_idx >= 0) ? df->ncols - 1 : df->ncols;

    TkDataframe *dst = df_new();
    if (!dst) return NULL;
    dst->nrows = df->nrows;
    dst->ncols = new_ncols;
    if (new_ncols == 0) {
        dst->cols = NULL;
        return dst;
    }
    dst->cols = calloc(new_ncols, sizeof(DfCol));
    if (!dst->cols) { df_free(dst); return NULL; }

    uint64_t di = 0;
    for (uint64_t j = 0; j < df->ncols; j++) {
        if ((int64_t)j == drop_idx) continue;
        DfCol *sc = &df->cols[j];
        DfCol *dc = &dst->cols[di];
        dc->name  = df_strdup(sc->name);
        dc->type  = sc->type;
        dc->nrows = sc->nrows;
        if (sc->type == DF_COL_F64) {
            dc->f64_data = malloc(sc->nrows * sizeof(double));
            if (!dc->f64_data) { df_free(dst); return NULL; }
            memcpy(dc->f64_data, sc->f64_data, sc->nrows * sizeof(double));
        } else {
            dc->str_data = malloc(sc->nrows * sizeof(char *));
            if (!dc->str_data) { df_free(dst); return NULL; }
            for (uint64_t r = 0; r < sc->nrows; r++)
                dc->str_data[r] = df_strdup(sc->str_data[r]);
        }
        di++;
    }
    return dst;
}

/* -------------------------------------------------------------------------
 * df_rename_column
 * ------------------------------------------------------------------------- */

TkDataframe *df_rename_column(TkDataframe *df, const char *old_name,
                               const char *new_name)
{
    if (!df || !old_name || !new_name) return NULL;

    TkDataframe *dst = df_new();
    if (!dst) return NULL;
    dst->nrows = df->nrows;
    dst->ncols = df->ncols;
    dst->cols  = calloc(df->ncols, sizeof(DfCol));
    if (!dst->cols) { df_free(dst); return NULL; }

    for (uint64_t j = 0; j < df->ncols; j++) {
        DfCol *sc = &df->cols[j];
        DfCol *dc = &dst->cols[j];
        /* Rename if this is the target column */
        int rename = (sc->name && strcmp(sc->name, old_name) == 0);
        dc->name  = df_strdup(rename ? new_name : sc->name);
        dc->type  = sc->type;
        dc->nrows = sc->nrows;
        if (sc->type == DF_COL_F64) {
            dc->f64_data = malloc(sc->nrows * sizeof(double));
            if (!dc->f64_data) { df_free(dst); return NULL; }
            memcpy(dc->f64_data, sc->f64_data, sc->nrows * sizeof(double));
        } else {
            dc->str_data = malloc(sc->nrows * sizeof(char *));
            if (!dc->str_data) { df_free(dst); return NULL; }
            for (uint64_t r = 0; r < sc->nrows; r++)
                dc->str_data[r] = df_strdup(sc->str_data[r]);
        }
    }
    return dst;
}

/* -------------------------------------------------------------------------
 * df_select_columns
 * ------------------------------------------------------------------------- */

TkDataframe *df_select_columns(TkDataframe *df, const char **cols,
                                uint64_t ncols)
{
    if (!df || !cols) return NULL;

    /* Count how many of the requested columns actually exist. */
    uint64_t found = 0;
    for (uint64_t i = 0; i < ncols; i++) {
        if (cols[i] && df_column(df, cols[i])) found++;
    }

    TkDataframe *dst = df_new();
    if (!dst) return NULL;
    dst->nrows = df->nrows;
    dst->ncols = found;
    if (found == 0) {
        dst->cols = NULL;
        return dst;
    }
    dst->cols = calloc(found, sizeof(DfCol));
    if (!dst->cols) { df_free(dst); return NULL; }

    uint64_t di = 0;
    for (uint64_t i = 0; i < ncols; i++) {
        if (!cols[i]) continue;
        DfCol *sc = df_column(df, cols[i]);
        if (!sc) continue;
        DfCol *dc = &dst->cols[di];
        dc->name  = df_strdup(sc->name);
        dc->type  = sc->type;
        dc->nrows = sc->nrows;
        if (sc->type == DF_COL_F64) {
            dc->f64_data = malloc(sc->nrows * sizeof(double));
            if (!dc->f64_data) { df_free(dst); return NULL; }
            memcpy(dc->f64_data, sc->f64_data, sc->nrows * sizeof(double));
        } else {
            dc->str_data = malloc(sc->nrows * sizeof(char *));
            if (!dc->str_data) { df_free(dst); return NULL; }
            for (uint64_t r = 0; r < sc->nrows; r++)
                dc->str_data[r] = df_strdup(sc->str_data[r]);
        }
        di++;
    }
    return dst;
}

/* -------------------------------------------------------------------------
 * df_value_counts
 * ------------------------------------------------------------------------- */

DfGroupResult df_value_counts(TkDataframe *df, const char *col)
{
    DfGroupResult empty = {NULL, 0};
    if (!df || !col) return empty;

    DfCol *c = df_column(df, col);
    if (!c || c->type != DF_COL_STR) return empty;

    /* Reuse the GroupAcc pattern from df_groupby. */
    typedef struct VcAcc {
        char          *key;
        uint64_t       count;
        struct VcAcc  *next;
    } VcAcc;

    uint64_t bkt_size = df->nrows * 2 + HT_INIT_SIZE;
    VcAcc **buckets = calloc(bkt_size, sizeof(VcAcc *));
    if (!buckets) return empty;
    uint64_t n_groups = 0;

    for (uint64_t r = 0; r < df->nrows; r++) {
        const char *key = c->str_data[r] ? c->str_data[r] : "";
        uint64_t h      = ht_hash(key, bkt_size);
        VcAcc *va       = buckets[h];
        while (va) {
            if (strcmp(va->key, key) == 0) break;
            va = va->next;
        }
        if (!va) {
            va = calloc(1, sizeof(VcAcc));
            if (!va) { free(buckets); return empty; }
            va->key    = df_strdup(key);
            va->next   = buckets[h];
            buckets[h] = va;
            n_groups++;
        }
        va->count++;
    }

    DfGroupRow *rows = malloc(n_groups * sizeof(DfGroupRow));
    if (!rows) { free(buckets); return empty; }

    uint64_t idx = 0;
    for (uint64_t i = 0; i < bkt_size && idx < n_groups; i++) {
        VcAcc *va = buckets[i];
        while (va) {
            rows[idx].group_val  = va->key;   /* transfer ownership */
            rows[idx].agg_result = (double)va->count;
            idx++;
            VcAcc *nx = va->next;
            free(va);
            va = nx;
        }
    }
    free(buckets);

    DfGroupResult gr;
    gr.rows  = rows;
    gr.nrows = n_groups;
    return gr;
}

/* =========================================================================
 * Story 31.1.2: df_concat, df_fillna, df_dropna, df_sample, df_get_row,
 *               df_to_html, df_to_csv
 * ========================================================================= */

/* -------------------------------------------------------------------------
 * df_concat
 * ------------------------------------------------------------------------- */

TkDataframe *df_concat(TkDataframe *left, TkDataframe *right)
{
    if (!left || !right) return NULL;

    /* Build the union of column names (left-first order). */
    uint64_t max_cols = left->ncols + right->ncols;
    char **names = malloc(max_cols * sizeof(char *));
    if (!names) return NULL;

    uint64_t ncols = 0;
    /* Add all left columns first. */
    for (uint64_t j = 0; j < left->ncols; j++)
        names[ncols++] = df_strdup(left->cols[j].name);

    /* Add right columns not already in left. */
    for (uint64_t j = 0; j < right->ncols; j++) {
        const char *rname = right->cols[j].name;
        int found = 0;
        for (uint64_t k = 0; k < left->ncols; k++) {
            if (left->cols[k].name && rname &&
                strcmp(left->cols[k].name, rname) == 0) {
                found = 1;
                break;
            }
        }
        if (!found)
            names[ncols++] = df_strdup(rname);
    }

    uint64_t nrows = left->nrows + right->nrows;

    TkDataframe *dst = df_new();
    if (!dst) {
        for (uint64_t i = 0; i < ncols; i++) free(names[i]);
        free(names);
        return NULL;
    }
    dst->ncols = ncols;
    dst->nrows = nrows;
    dst->cols  = calloc(ncols, sizeof(DfCol));
    if (!dst->cols) {
        for (uint64_t i = 0; i < ncols; i++) free(names[i]);
        free(names);
        df_free(dst);
        return NULL;
    }

    for (uint64_t j = 0; j < ncols; j++) {
        DfCol *dc = &dst->cols[j];
        dc->name  = names[j]; /* transfer ownership */
        /* Determine type: prefer the left frame's type; if the column exists
         * only in right, use right's type; if in neither, use STR. */
        DfCol *lc = df_column(left, names[j]);
        DfCol *rc = df_column(right, names[j]);
        DfColType typ = DF_COL_STR;
        if (lc) typ = lc->type;
        else if (rc) typ = rc->type;
        dc->type  = typ;
        dc->nrows = nrows;

        if (typ == DF_COL_F64) {
            dc->f64_data = malloc(nrows * sizeof(double));
            if (!dc->f64_data) { free(names); df_free(dst); return NULL; }
            for (uint64_t r = 0; r < left->nrows; r++)
                dc->f64_data[r] = lc ? lc->f64_data[r] : 0.0;
            for (uint64_t r = 0; r < right->nrows; r++) {
                if (rc)
                    dc->f64_data[left->nrows + r] = rc->f64_data[r];
                else
                    dc->f64_data[left->nrows + r] = 0.0;
            }
        } else {
            dc->str_data = malloc(nrows * sizeof(char *));
            if (!dc->str_data) { free(names); df_free(dst); return NULL; }
            for (uint64_t r = 0; r < left->nrows; r++) {
                if (lc && lc->str_data)
                    dc->str_data[r] = df_strdup(lc->str_data[r]);
                else
                    dc->str_data[r] = df_strdup("");
            }
            for (uint64_t r = 0; r < right->nrows; r++) {
                if (rc && rc->str_data)
                    dc->str_data[left->nrows + r] = df_strdup(rc->str_data[r]);
                else
                    dc->str_data[left->nrows + r] = df_strdup("");
            }
        }
    }

    free(names); /* individual strings were transferred into cols */
    return dst;
}

/* -------------------------------------------------------------------------
 * df_fillna
 * ------------------------------------------------------------------------- */

TkDataframe *df_fillna(TkDataframe *df, const char *col, const char *value)
{
    if (!df || !col || !value) return NULL;
    DfCol *sc = df_column(df, col);
    if (!sc || sc->type != DF_COL_STR) return NULL;

    /* Build a full copy of the dataframe, substituting missing values. */
    TkDataframe *dst = df_new();
    if (!dst) return NULL;
    dst->ncols = df->ncols;
    dst->nrows = df->nrows;
    dst->cols  = calloc(df->ncols, sizeof(DfCol));
    if (!dst->cols) { df_free(dst); return NULL; }

    for (uint64_t j = 0; j < df->ncols; j++) {
        DfCol *src = &df->cols[j];
        DfCol *dc  = &dst->cols[j];
        int is_target = (src->name && strcmp(src->name, col) == 0);

        dc->name  = df_strdup(src->name);
        dc->type  = src->type;
        dc->nrows = src->nrows;

        if (src->type == DF_COL_F64) {
            dc->f64_data = malloc(src->nrows * sizeof(double));
            if (!dc->f64_data) { df_free(dst); return NULL; }
            memcpy(dc->f64_data, src->f64_data, src->nrows * sizeof(double));
        } else {
            dc->str_data = malloc(src->nrows * sizeof(char *));
            if (!dc->str_data) { df_free(dst); return NULL; }
            for (uint64_t r = 0; r < src->nrows; r++) {
                const char *v = src->str_data ? src->str_data[r] : NULL;
                if (is_target && (!v || *v == '\0'))
                    dc->str_data[r] = df_strdup(value);
                else
                    dc->str_data[r] = df_strdup(v ? v : "");
            }
        }
    }
    return dst;
}

/* -------------------------------------------------------------------------
 * df_dropna
 * ------------------------------------------------------------------------- */

TkDataframe *df_dropna(TkDataframe *df, const char *col)
{
    if (!df || !col) return NULL;
    DfCol *c = df_column(df, col);
    if (!c || c->type != DF_COL_STR) return NULL;

    int *mask = calloc(df->nrows, sizeof(int));
    if (!mask) return NULL;

    uint64_t new_nrows = 0;
    for (uint64_t r = 0; r < df->nrows; r++) {
        const char *v = c->str_data ? c->str_data[r] : NULL;
        if (v && *v != '\0') {
            mask[r] = 1;
            new_nrows++;
        }
    }

    TkDataframe *result = copy_rows_subset(df, mask, new_nrows);
    free(mask);
    return result;
}

/* -------------------------------------------------------------------------
 * df_sample  (Fisher-Yates with LCG)
 * ------------------------------------------------------------------------- */

TkDataframe *df_sample(TkDataframe *df, uint64_t n, uint64_t seed)
{
    if (!df) return NULL;
    uint64_t nrows = df->nrows;
    if (nrows == 0) return df_head(df, 0);

    /* Build index array [0, nrows-1]. */
    uint64_t *idx = malloc(nrows * sizeof(uint64_t));
    if (!idx) return NULL;
    for (uint64_t r = 0; r < nrows; r++) idx[r] = r;

    /* Fisher-Yates shuffle with LCG random number generator.
     * LCG parameters from Numerical Recipes (Knuth). */
    uint64_t state = seed ^ 0xdeadbeefcafeULL;
    uint64_t take  = (n < nrows) ? n : nrows;

    for (uint64_t i = 0; i < take; i++) {
        /* Advance LCG state */
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        /* Map to [i, nrows-1] */
        uint64_t range = nrows - i;
        uint64_t j     = i + (state % range);
        /* Swap */
        uint64_t tmp = idx[i];
        idx[i] = idx[j];
        idx[j] = tmp;
    }

    /* Build a new dataframe using the first take indices. */
    TkDataframe *dst = df_new();
    if (!dst) { free(idx); return NULL; }
    dst->ncols = df->ncols;
    dst->nrows = take;
    dst->cols  = calloc(df->ncols, sizeof(DfCol));
    if (!dst->cols) { free(idx); df_free(dst); return NULL; }

    for (uint64_t j = 0; j < df->ncols; j++) {
        DfCol *sc = &df->cols[j];
        DfCol *dc = &dst->cols[j];
        dc->name  = df_strdup(sc->name);
        dc->type  = sc->type;
        dc->nrows = take;

        if (sc->type == DF_COL_F64) {
            dc->f64_data = malloc(take * sizeof(double));
            if (!dc->f64_data) { free(idx); df_free(dst); return NULL; }
            for (uint64_t r = 0; r < take; r++)
                dc->f64_data[r] = sc->f64_data[idx[r]];
        } else {
            dc->str_data = malloc(take * sizeof(char *));
            if (!dc->str_data) { free(idx); df_free(dst); return NULL; }
            for (uint64_t r = 0; r < take; r++)
                dc->str_data[r] = df_strdup(
                    sc->str_data ? (sc->str_data[idx[r]] ? sc->str_data[idx[r]] : "") : "");
        }
    }

    free(idx);
    return dst;
}

/* -------------------------------------------------------------------------
 * df_get_row
 * ------------------------------------------------------------------------- */

DfResult df_get_row(TkDataframe *df, uint64_t idx)
{
    DfResult res = {NULL, 0, NULL};
    if (!df) {
        res.is_err  = 1;
        res.err_msg = "df_get_row: NULL dataframe";
        return res;
    }
    if (idx >= df->nrows) {
        res.is_err  = 1;
        res.err_msg = "df_get_row: index out of bounds";
        return res;
    }

    int *mask = calloc(df->nrows, sizeof(int));
    if (!mask) {
        res.is_err  = 1;
        res.err_msg = "df_get_row: out of memory";
        return res;
    }
    mask[idx] = 1;
    TkDataframe *row = copy_rows_subset(df, mask, 1);
    free(mask);
    if (!row) {
        res.is_err  = 1;
        res.err_msg = "df_get_row: out of memory";
        return res;
    }
    res.ok = row;
    return res;
}

/* -------------------------------------------------------------------------
 * df_to_html
 * ------------------------------------------------------------------------- */

/* Append s to sb with HTML escaping of <, >, &, ", '. */
static void sb_append_html(DfBuf *sb, const char *s)
{
    if (!s) return;
    for (; *s; s++) {
        switch (*s) {
            case '<':  sb_append(sb, "&lt;");   break;
            case '>':  sb_append(sb, "&gt;");   break;
            case '&':  sb_append(sb, "&amp;");  break;
            case '"':  sb_append(sb, "&quot;"); break;
            case '\'': sb_append(sb, "&#39;");  break;
            default: {
                char tmp[2] = { *s, '\0' };
                sb_append(sb, tmp);
                break;
            }
        }
    }
}

const char *df_to_html(TkDataframe *df)
{
    if (!df) return NULL;

    DfBuf sb;
    if (sb_init(&sb) != 0) return NULL;

    sb_append(&sb, "<table>\n<thead>\n<tr>");
    for (uint64_t j = 0; j < df->ncols; j++) {
        sb_append(&sb, "<th>");
        sb_append_html(&sb, df->cols[j].name ? df->cols[j].name : "");
        sb_append(&sb, "</th>");
    }
    sb_append(&sb, "</tr>\n</thead>\n<tbody>\n");

    char numtmp[64];
    for (uint64_t r = 0; r < df->nrows; r++) {
        sb_append(&sb, "<tr>");
        for (uint64_t j = 0; j < df->ncols; j++) {
            DfCol *c = &df->cols[j];
            sb_append(&sb, "<td>");
            if (c->type == DF_COL_F64) {
                snprintf(numtmp, sizeof(numtmp), "%.17g", c->f64_data[r]);
                sb_append(&sb, numtmp);
            } else {
                const char *v = (c->str_data && c->str_data[r]) ? c->str_data[r] : "";
                sb_append_html(&sb, v);
            }
            sb_append(&sb, "</td>");
        }
        sb_append(&sb, "</tr>\n");
    }

    sb_append(&sb, "</tbody>\n</table>");
    return sb.buf; /* caller owns */
}

/* -------------------------------------------------------------------------
 * df_to_csv  (alias for df_tocsv)
 * ------------------------------------------------------------------------- */

const char *df_to_csv(TkDataframe *df)
{
    return df_tocsv(df);
}

/* =========================================================================
 * df_schema  (return column metadata)
 * ========================================================================= */

DfSeries *df_schema(TkDataframe *df, uint64_t *out_len)
{
    if (out_len) *out_len = 0;
    if (!df) return NULL;

    DfSeries *series = malloc(df->ncols * sizeof(DfSeries));
    if (!series) return NULL;

    for (uint64_t j = 0; j < df->ncols; j++) {
        series[j].name  = df_strdup(df->cols[j].name);
        series[j].dtype = df_strdup(
            df->cols[j].type == DF_COL_F64 ? "f64" : "str");
        series[j].len   = df->cols[j].nrows;
    }

    if (out_len) *out_len = df->ncols;
    return series;
}
