/*
 * vecstore.c — Implementation of the std.vecstore standard library module.
 *
 * Flat-index vector store with brute-force cosine similarity search.
 * Vectors are normalised to unit length on upsert; search reduces to a
 * dot product of normalised vectors.
 *
 * No external dependencies beyond libc and pthreads.
 * Does NOT include <math.h> to avoid shadowing by the local src/stdlib/math.h.
 * Square roots are computed via a Newton-Raphson inline helper (vs_sqrt).
 *
 * File format: {data_dir}/{name}.vecs — see vecstore.h documentation.
 *
 * Story: 72.5.3
 */

#include "vecstore.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <pthread.h>

/* -------------------------------------------------------------------------
 * Internal constants
 * ------------------------------------------------------------------------- */

#define VECS_MAGIC    0x544B5643u  /* "TKVC" little-endian */
#define VECS_VERSION  1u
#define INIT_CAPACITY 16
#define MAX_NAME_LEN  256
#define PATH_SEP      '/'

/* -------------------------------------------------------------------------
 * Inline sqrt via Newton-Raphson (avoids <math.h> dependency)
 * Accurate to machine epsilon for float/double.
 * Returns 0.0 for x <= 0.
 * ------------------------------------------------------------------------- */

static double vs_sqrt(double x)
{
    if (x <= 0.0) return 0.0;
    /* Initial estimate using bit-manipulation on double representation.
     * Relies on IEEE 754 layout: exponent bias 1023, mantissa 52 bits. */
    double r;
    {
        uint64_t bits;
        memcpy(&bits, &x, sizeof bits);
        bits = (uint64_t)((bits >> 1) + (UINT64_C(1023) << 51));
        memcpy(&r, &bits, sizeof r);
    }
    /* Four Newton-Raphson iterations: r = (r + x/r) / 2 */
    r = 0.5 * (r + x / r);
    r = 0.5 * (r + x / r);
    r = 0.5 * (r + x / r);
    r = 0.5 * (r + x / r);
    return r;
}

/* -------------------------------------------------------------------------
 * Internal data structures
 * ------------------------------------------------------------------------- */

/* One stored entry. */
typedef struct {
    char    *id;          /* heap-allocated, null-terminated */
    float   *embedding;   /* normalised unit vector, dim floats */
    int32_t  dim;
    char    *payload;     /* heap-allocated, null-terminated */
    int64_t  created_at;  /* Unix seconds */
} VecEntry;

/* One collection (dynamic array of VecEntry). */
struct TkVecCollection {
    char     *name;       /* heap-allocated */
    VecEntry *entries;    /* heap-allocated array */
    int32_t   count;
    int32_t   capacity;
    int32_t   dim;        /* locked in on first upsert; 0 = unset */
    int       dirty;      /* 1 if unsaved changes exist */
    char     *file_path;  /* heap-allocated full path */
};

/* The store itself — holds a list of open collections. */
struct TkVecStore {
    char              *data_dir;      /* heap-allocated */
    TkVecCollection  **collections;   /* heap-allocated pointer array */
    int32_t            ncol;
    int32_t            col_cap;
    pthread_mutex_t    mutex;
};

/* -------------------------------------------------------------------------
 * Internal helpers
 * ------------------------------------------------------------------------- */

/* Compute dot product of two float vectors of length dim. */
static double dot_product(const float *a, const float *b, int32_t dim)
{
    double sum = 0.0;
    for (int32_t i = 0; i < dim; i++) {
        sum += (double)a[i] * (double)b[i];
    }
    return sum;
}

/* Normalise src (length dim) into dst.
 * Returns 1 on success, 0 if the vector is the zero vector. */
static int normalise(float *dst, const float *src, int32_t dim)
{
    double sq = 0.0;
    for (int32_t i = 0; i < dim; i++) {
        sq += (double)src[i] * (double)src[i];
    }
    if (sq == 0.0) return 0;
    double inv_norm = 1.0 / vs_sqrt(sq);
    for (int32_t i = 0; i < dim; i++) {
        dst[i] = (float)((double)src[i] * inv_norm);
    }
    return 1;
}

/* Build the file path for a collection: {data_dir}/{name}.vecs */
static char *make_file_path(const char *data_dir, const char *name)
{
    size_t dlen = strlen(data_dir);
    size_t nlen = strlen(name);
    /* data_dir + "/" + name + ".vecs" + NUL */
    size_t total = dlen + 1 + nlen + 5 + 1;
    char *path = (char *)malloc(total);
    if (!path) return NULL;
    int need_sep = (dlen > 0 && data_dir[dlen - 1] != PATH_SEP) ? 1 : 0;
    if (need_sep) {
        snprintf(path, total, "%s/%s.vecs", data_dir, name);
    } else {
        snprintf(path, total, "%s%s.vecs", data_dir, name);
    }
    return path;
}

/* Free a single VecEntry's heap members (not the entry struct itself). */
static void entry_free_members(VecEntry *e)
{
    free(e->id);
    free(e->embedding);
    free(e->payload);
    e->id        = NULL;
    e->embedding = NULL;
    e->payload   = NULL;
}

/* Free a TkVecCollection and all its entries. */
static void collection_free(TkVecCollection *col)
{
    if (!col) return;
    for (int32_t i = 0; i < col->count; i++) {
        entry_free_members(&col->entries[i]);
    }
    free(col->entries);
    free(col->name);
    free(col->file_path);
    free(col);
}

/* -------------------------------------------------------------------------
 * File I/O: load and save
 * ------------------------------------------------------------------------- */

/*
 * File layout (native byte order):
 *
 *   Header (24 bytes):
 *     magic    u32   VECS_MAGIC
 *     version  u32   VECS_VERSION
 *     count    u32
 *     dim      u32
 *     _pad     u64   (zero)
 *
 *   Per-entry (variable):
 *     id_len   u32
 *     id       u8[id_len]    (no NUL)
 *     pay_len  u32
 *     payload  u8[pay_len]   (no NUL)
 *     ts       i64
 *     floats   f32[dim]
 */

static int collection_save(TkVecCollection *col)
{
    if (!col || !col->file_path) return 0;
    FILE *fp = fopen(col->file_path, "wb");
    if (!fp) return 0;

    uint32_t magic   = VECS_MAGIC;
    uint32_t version = VECS_VERSION;
    uint32_t count   = (uint32_t)col->count;
    uint32_t dim     = (uint32_t)(col->dim > 0 ? col->dim : 0);
    uint64_t pad     = 0;

    if (fwrite(&magic,   sizeof magic,   1, fp) != 1) { fclose(fp); return 0; }
    if (fwrite(&version, sizeof version, 1, fp) != 1) { fclose(fp); return 0; }
    if (fwrite(&count,   sizeof count,   1, fp) != 1) { fclose(fp); return 0; }
    if (fwrite(&dim,     sizeof dim,     1, fp) != 1) { fclose(fp); return 0; }
    if (fwrite(&pad,     sizeof pad,     1, fp) != 1) { fclose(fp); return 0; }

    for (int32_t i = 0; i < col->count; i++) {
        VecEntry *e = &col->entries[i];
        uint32_t id_len  = (uint32_t)strlen(e->id);
        uint32_t pay_len = (uint32_t)strlen(e->payload);

        if (fwrite(&id_len,     sizeof id_len,  1,        fp) != 1)         { fclose(fp); return 0; }
        if (fwrite(e->id,       1,              id_len,   fp) != id_len)    { fclose(fp); return 0; }
        if (fwrite(&pay_len,    sizeof pay_len, 1,        fp) != 1)         { fclose(fp); return 0; }
        if (fwrite(e->payload,  1,              pay_len,  fp) != pay_len)   { fclose(fp); return 0; }
        if (fwrite(&e->created_at, sizeof e->created_at, 1, fp) != 1)       { fclose(fp); return 0; }
        if (dim > 0) {
            if (fwrite(e->embedding, sizeof(float), (size_t)dim, fp) != (size_t)dim) { fclose(fp); return 0; }
        }
    }

    fclose(fp);
    return 1;
}

static int collection_load(TkVecCollection *col)
{
    if (!col || !col->file_path) return 0;
    FILE *fp = fopen(col->file_path, "rb");
    if (!fp) return 1; /* no file yet is OK — empty collection */

    uint32_t magic   = 0;
    uint32_t version = 0;
    uint32_t count   = 0;
    uint32_t dim     = 0;
    uint64_t pad     = 0;

    if (fread(&magic,   sizeof magic,   1, fp) != 1) { fclose(fp); return 0; }
    if (fread(&version, sizeof version, 1, fp) != 1) { fclose(fp); return 0; }
    if (fread(&count,   sizeof count,   1, fp) != 1) { fclose(fp); return 0; }
    if (fread(&dim,     sizeof dim,     1, fp) != 1) { fclose(fp); return 0; }
    if (fread(&pad,     sizeof pad,     1, fp) != 1) { fclose(fp); return 0; }

    if (magic != VECS_MAGIC || version != VECS_VERSION) { fclose(fp); return 0; }

    if (count == 0) { fclose(fp); return 1; }

    col->dim = (int32_t)dim;

    /* Allocate entry array */
    int32_t new_cap = (int32_t)count;
    if (new_cap < INIT_CAPACITY) new_cap = INIT_CAPACITY;
    VecEntry *entries = (VecEntry *)calloc((size_t)new_cap, sizeof(VecEntry));
    if (!entries) { fclose(fp); return 0; }

    int32_t loaded = 0;
    for (uint32_t i = 0; i < count; i++) {
        uint32_t id_len  = 0;
        uint32_t pay_len = 0;

        if (fread(&id_len, sizeof id_len, 1, fp) != 1) goto load_err;

        char *id = (char *)malloc((size_t)id_len + 1);
        if (!id) goto load_err;
        if (id_len > 0 && fread(id, 1, (size_t)id_len, fp) != (size_t)id_len) { free(id); goto load_err; }
        id[id_len] = '\0';

        if (fread(&pay_len, sizeof pay_len, 1, fp) != 1) { free(id); goto load_err; }

        char *payload = (char *)malloc((size_t)pay_len + 1);
        if (!payload) { free(id); goto load_err; }
        if (pay_len > 0 && fread(payload, 1, (size_t)pay_len, fp) != (size_t)pay_len) { free(id); free(payload); goto load_err; }
        payload[pay_len] = '\0';

        int64_t ts = 0;
        if (fread(&ts, sizeof ts, 1, fp) != 1) { free(id); free(payload); goto load_err; }

        float *emb = NULL;
        if (dim > 0) {
            emb = (float *)malloc(sizeof(float) * dim);
            if (!emb) { free(id); free(payload); goto load_err; }
            if (fread(emb, sizeof(float), (size_t)dim, fp) != (size_t)dim) { free(id); free(payload); free(emb); goto load_err; }
        }

        entries[loaded].id         = id;
        entries[loaded].embedding  = emb;
        entries[loaded].dim        = (int32_t)dim;
        entries[loaded].payload    = payload;
        entries[loaded].created_at = ts;
        loaded++;
    }

    fclose(fp);
    col->entries  = entries;
    col->count    = loaded;
    col->capacity = new_cap;
    col->dirty    = 0;
    return 1;

load_err:
    for (int32_t j = 0; j < loaded; j++) {
        entry_free_members(&entries[j]);
    }
    free(entries);
    fclose(fp);
    return 0;
}

/* -------------------------------------------------------------------------
 * Store lifecycle
 * ------------------------------------------------------------------------- */

TkVecStore *vecstore_open(const char *data_dir)
{
    if (!data_dir || data_dir[0] == '\0') return NULL;

    TkVecStore *vs = (TkVecStore *)calloc(1, sizeof(TkVecStore));
    if (!vs) return NULL;

    vs->data_dir = (char *)malloc(strlen(data_dir) + 1);
    if (!vs->data_dir) { free(vs); return NULL; }
    strcpy(vs->data_dir, data_dir);

    vs->col_cap    = INIT_CAPACITY;
    vs->ncol       = 0;
    vs->collections = (TkVecCollection **)calloc((size_t)vs->col_cap,
                                                  sizeof(TkVecCollection *));
    if (!vs->collections) {
        free(vs->data_dir);
        free(vs);
        return NULL;
    }

    if (pthread_mutex_init(&vs->mutex, NULL) != 0) {
        free(vs->collections);
        free(vs->data_dir);
        free(vs);
        return NULL;
    }

    return vs;
}

void vecstore_close(TkVecStore *vs)
{
    if (!vs) return;

    pthread_mutex_lock(&vs->mutex);

    for (int32_t i = 0; i < vs->ncol; i++) {
        TkVecCollection *col = vs->collections[i];
        if (col && col->dirty) {
            collection_save(col);
        }
        collection_free(col);
        vs->collections[i] = NULL;
    }

    free(vs->collections);
    free(vs->data_dir);

    pthread_mutex_unlock(&vs->mutex);
    pthread_mutex_destroy(&vs->mutex);

    free(vs);
}

/* -------------------------------------------------------------------------
 * Collection operations
 * ------------------------------------------------------------------------- */

TkVecCollection *vecstore_collection(TkVecStore *vs, const char *name)
{
    if (!vs || !name || name[0] == '\0') return NULL;

    /* Reject names with path separators */
    if (strchr(name, '/') || strchr(name, '\\')) return NULL;

    pthread_mutex_lock(&vs->mutex);

    /* Check if already open */
    for (int32_t i = 0; i < vs->ncol; i++) {
        if (vs->collections[i] && strcmp(vs->collections[i]->name, name) == 0) {
            TkVecCollection *found = vs->collections[i];
            pthread_mutex_unlock(&vs->mutex);
            return found;
        }
    }

    /* Create new collection */
    TkVecCollection *col = (TkVecCollection *)calloc(1, sizeof(TkVecCollection));
    if (!col) { pthread_mutex_unlock(&vs->mutex); return NULL; }

    col->name = (char *)malloc(strlen(name) + 1);
    if (!col->name) { free(col); pthread_mutex_unlock(&vs->mutex); return NULL; }
    strcpy(col->name, name);

    col->file_path = make_file_path(vs->data_dir, name);
    if (!col->file_path) { free(col->name); free(col); pthread_mutex_unlock(&vs->mutex); return NULL; }

    col->capacity = INIT_CAPACITY;
    col->count    = 0;
    col->dim      = 0;
    col->dirty    = 0;
    col->entries  = (VecEntry *)calloc((size_t)col->capacity, sizeof(VecEntry));
    if (!col->entries) {
        free(col->file_path);
        free(col->name);
        free(col);
        pthread_mutex_unlock(&vs->mutex);
        return NULL;
    }

    /* Load from file if it exists */
    if (!collection_load(col)) {
        collection_free(col);
        pthread_mutex_unlock(&vs->mutex);
        return NULL;
    }

    /* Register with store */
    if (vs->ncol == vs->col_cap) {
        int32_t new_cap = vs->col_cap * 2;
        TkVecCollection **tmp = (TkVecCollection **)realloc(
            vs->collections,
            (size_t)new_cap * sizeof(TkVecCollection *)
        );
        if (!tmp) {
            collection_free(col);
            pthread_mutex_unlock(&vs->mutex);
            return NULL;
        }
        vs->collections = tmp;
        vs->col_cap     = new_cap;
    }
    vs->collections[vs->ncol++] = col;

    pthread_mutex_unlock(&vs->mutex);
    return col;
}

int vecstore_upsert(TkVecCollection *col,
                    const char      *id,
                    const float     *embedding,
                    int32_t          dim,
                    const char      *payload)
{
    if (!col || !id || !embedding || dim < 1 || !payload) return 0;

    /* Lock: find the parent store's mutex by convention — the collection does
     * not hold a back-pointer to the store.  We use the collection's own lock
     * embedded in the store; since collections are only accessed via a store
     * we rely on callers to use a single store per thread or higher-level
     * coordination.  For intra-store safety, operations on different
     * collections are independent: each collection is only registered once and
     * accessed through the same store lock path.
     *
     * For simplicity and correctness, the public API locks at the store level
     * (in vecstore_collection).  Individual collection functions do not
     * re-acquire the store mutex to avoid requiring a back-pointer.  This
     * implementation is safe when:
     *   a) a single thread uses a given collection, or
     *   b) the caller synchronises externally.
     *
     * Story 72.5 does not require intra-collection concurrent writes. */

    /* Validate / lock in dimension */
    if (col->dim == 0) {
        col->dim = dim;
    } else if (col->dim != dim) {
        return 0; /* dimension mismatch */
    }

    /* Normalise the embedding */
    float *normed = (float *)malloc(sizeof(float) * (size_t)dim);
    if (!normed) return 0;
    if (!normalise(normed, embedding, dim)) {
        free(normed); /* zero vector */
        return 0;
    }

    /* Check for existing entry with same id → replace */
    for (int32_t i = 0; i < col->count; i++) {
        if (strcmp(col->entries[i].id, id) == 0) {
            /* Replace in-place */
            free(col->entries[i].embedding);
            free(col->entries[i].payload);
            col->entries[i].embedding  = normed;
            col->entries[i].dim        = dim;
            col->entries[i].payload    = (char *)malloc(strlen(payload) + 1);
            if (!col->entries[i].payload) {
                col->entries[i].embedding = NULL; /* best-effort */
                return 0;
            }
            strcpy(col->entries[i].payload, payload);
            col->entries[i].created_at = (int64_t)time(NULL);
            col->dirty = 1;
            return 1;
        }
    }

    /* Grow if needed */
    if (col->count == col->capacity) {
        int32_t new_cap = col->capacity * 2;
        VecEntry *tmp = (VecEntry *)realloc(col->entries,
                                             (size_t)new_cap * sizeof(VecEntry));
        if (!tmp) { free(normed); return 0; }
        col->entries  = tmp;
        col->capacity = new_cap;
    }

    /* Insert */
    VecEntry *e   = &col->entries[col->count];
    e->id         = (char *)malloc(strlen(id) + 1);
    e->payload    = (char *)malloc(strlen(payload) + 1);
    if (!e->id || !e->payload) {
        free(e->id);
        free(e->payload);
        free(normed);
        return 0;
    }
    strcpy(e->id,      id);
    strcpy(e->payload, payload);
    e->embedding  = normed;
    e->dim        = dim;
    e->created_at = (int64_t)time(NULL);
    col->count++;
    col->dirty = 1;
    return 1;
}

/* Simple insertion sort used by search to keep top_k results. */
static void insert_sorted(TkSearchResult *arr, int32_t *n, int32_t cap,
                           double score, const char *id, const char *payload)
{
    /* Only insert if list is not full or score > minimum */
    if (*n < cap) {
        /* Append and bubble up */
        int32_t pos = *n;
        arr[pos].score   = score;
        arr[pos].id      = (char *)malloc(strlen(id) + 1);
        arr[pos].payload = (char *)malloc(strlen(payload) + 1);
        if (!arr[pos].id || !arr[pos].payload) {
            free(arr[pos].id);
            free(arr[pos].payload);
            return;
        }
        strcpy(arr[pos].id,      id);
        strcpy(arr[pos].payload, payload);
        (*n)++;
        /* Bubble up (insertion sort descending) */
        while (pos > 0 && arr[pos].score > arr[pos - 1].score) {
            TkSearchResult tmp = arr[pos];
            arr[pos]           = arr[pos - 1];
            arr[pos - 1]       = tmp;
            pos--;
        }
    } else if (*n > 0 && score > arr[*n - 1].score) {
        /* Replace last (lowest) entry */
        free(arr[*n - 1].id);
        free(arr[*n - 1].payload);
        arr[*n - 1].score   = score;
        arr[*n - 1].id      = (char *)malloc(strlen(id) + 1);
        arr[*n - 1].payload = (char *)malloc(strlen(payload) + 1);
        if (!arr[*n - 1].id || !arr[*n - 1].payload) {
            free(arr[*n - 1].id);
            free(arr[*n - 1].payload);
            arr[*n - 1].id      = NULL;
            arr[*n - 1].payload = NULL;
            return;
        }
        strcpy(arr[*n - 1].id,      id);
        strcpy(arr[*n - 1].payload, payload);
        /* Bubble up */
        int32_t pos = *n - 1;
        while (pos > 0 && arr[pos].score > arr[pos - 1].score) {
            TkSearchResult tmp = arr[pos];
            arr[pos]           = arr[pos - 1];
            arr[pos - 1]       = tmp;
            pos--;
        }
    }
}

TkSearchResultArray vecstore_search(TkVecCollection *col,
                                    const float     *query,
                                    int32_t          dim,
                                    int32_t          top_k,
                                    double           min_score)
{
    TkSearchResultArray out = { NULL, 0 };
    if (!col || !query || dim < 1 || top_k < 1) return out;
    if (col->count == 0) return out;
    if (col->dim != dim) return out;

    /* Normalise query */
    float *q_norm = (float *)malloc(sizeof(float) * (size_t)dim);
    if (!q_norm) return out;
    if (!normalise(q_norm, query, dim)) {
        free(q_norm);
        return out; /* zero query vector */
    }

    /* Allocate result buffer */
    TkSearchResult *results = (TkSearchResult *)calloc((size_t)top_k,
                                                        sizeof(TkSearchResult));
    if (!results) { free(q_norm); return out; }

    int32_t found = 0;

    for (int32_t i = 0; i < col->count; i++) {
        VecEntry *e = &col->entries[i];
        if (!e->embedding || e->dim != dim) continue;
        double score = dot_product(e->embedding, q_norm, dim);
        if (score < min_score) continue;
        insert_sorted(results, &found, top_k, score, e->id, e->payload);
    }

    free(q_norm);

    /* Trim array to actual count */
    if (found == 0) {
        free(results);
        return out;
    }

    out.items = results;
    out.count = found;
    return out;
}

int vecstore_delete(TkVecCollection *col, const char *id)
{
    if (!col || !id) return 0;
    for (int32_t i = 0; i < col->count; i++) {
        if (strcmp(col->entries[i].id, id) == 0) {
            entry_free_members(&col->entries[i]);
            /* Shift remaining entries left */
            for (int32_t j = i; j < col->count - 1; j++) {
                col->entries[j] = col->entries[j + 1];
            }
            col->count--;
            col->dirty = 1;
            return 1;
        }
    }
    return 0;
}

int32_t vecstore_delete_before(TkVecCollection *col, int64_t before_ts)
{
    if (!col) return 0;
    int32_t removed = 0;
    int32_t i = 0;
    while (i < col->count) {
        if (col->entries[i].created_at < before_ts) {
            entry_free_members(&col->entries[i]);
            for (int32_t j = i; j < col->count - 1; j++) {
                col->entries[j] = col->entries[j + 1];
            }
            col->count--;
            removed++;
            col->dirty = 1;
            /* do not increment i — re-examine slot i */
        } else {
            i++;
        }
    }
    return removed;
}

int32_t vecstore_count(TkVecCollection *col)
{
    if (!col) return 0;
    return col->count;
}

/* -------------------------------------------------------------------------
 * Memory management
 * ------------------------------------------------------------------------- */

void vecstore_free_results(TkSearchResultArray *results)
{
    if (!results || !results->items) return;
    for (int32_t i = 0; i < results->count; i++) {
        free(results->items[i].id);
        free(results->items[i].payload);
    }
    free(results->items);
    results->items = NULL;
    results->count = 0;
}
