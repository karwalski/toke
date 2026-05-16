/*
 * vecstore_glue.c — i64-ABI wrappers for std.vecstore module.
 *
 * Vector store for semantic search. Backed by a local flat-index with
 * cosine similarity.
 */

#include "vecstore.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* vecstore.open(path) — open or create a vector store at path */
int64_t tk_vecstore_open_w(int64_t path) {
    if (!path) return 0;
    TkVecStore *vs = vecstore_open((const char *)(intptr_t)path);
    return (int64_t)(intptr_t)vs;
}

/* vecstore.collection(store, name) — get or create a named collection */
int64_t tk_vecstore_collection_w(int64_t store, int64_t name) {
    if (!store || !name) return 0;
    TkVecStore *vs = (TkVecStore *)(intptr_t)store;
    TkVecCollection *col = vecstore_collection(vs, (const char *)(intptr_t)name);
    return (int64_t)(intptr_t)col;
}

/* vecstore.upsert(collection, id, embedding, metadata) — insert/update vector */
int64_t tk_vecstore_upsert_w(int64_t coll, int64_t id, int64_t embedding, int64_t metadata) {
    if (!coll || !id) return 0;
    TkVecCollection *vc = (TkVecCollection *)(intptr_t)coll;
    /* Decode toke f64 array into float array for vecstore */
    float *vec = NULL;
    int32_t dim = 0;
    if (embedding) {
        int64_t *ptr = (int64_t *)(intptr_t)embedding;
        int64_t count = ptr[-1];
        dim = (int32_t)count;
        vec = (float *)malloc((size_t)dim * sizeof(float));
        if (vec) {
            for (int32_t i = 0; i < dim; i++) {
                double d;
                memcpy(&d, &ptr[i], sizeof(d));
                vec[i] = (float)d;
            }
        }
    }
    int ok = vecstore_upsert(vc,
        (const char *)(intptr_t)id,
        vec, dim,
        metadata ? (const char *)(intptr_t)metadata : "");
    free(vec);
    return (int64_t)ok;
}

/* vecstore.search(collection, query_vec, k) — find k nearest neighbors */
int64_t tk_vecstore_search_w(int64_t coll, int64_t query_vec, int64_t k) {
    if (!coll || !query_vec || k <= 0) return 0;
    TkVecCollection *vc = (TkVecCollection *)(intptr_t)coll;
    /* Decode query vector */
    int64_t *ptr = (int64_t *)(intptr_t)query_vec;
    int64_t count = ptr[-1];
    int32_t dim = (int32_t)count;
    float *vec = (float *)malloc((size_t)dim * sizeof(float));
    if (!vec) return 0;
    for (int32_t i = 0; i < dim; i++) {
        double d;
        memcpy(&d, &ptr[i], sizeof(d));
        vec[i] = (float)d;
    }
    TkSearchResultArray results = vecstore_search(vc, vec, dim, (int32_t)k, 0.0);
    free(vec);
    if (results.count <= 0) return 0;
    /* Return as toke array of id strings */
    int64_t *block = (int64_t *)malloc(((size_t)results.count + 1) * sizeof(int64_t));
    if (!block) { vecstore_free_results(&results); return 0; }
    block[0] = (int64_t)results.count;
    for (int32_t i = 0; i < results.count; i++)
        block[i + 1] = (int64_t)(intptr_t)results.items[i].id;
    /* Don't free results — ids are now owned by the toke array */
    return (int64_t)(intptr_t)(block + 1);
}

/* vecstore.delete(collection, id) — remove a vector by id */
int64_t tk_vecstore_delete_w(int64_t coll, int64_t id) {
    if (!coll || !id) return 0;
    TkVecCollection *vc = (TkVecCollection *)(intptr_t)coll;
    return (int64_t)vecstore_delete(vc, (const char *)(intptr_t)id);
}

/* vecstore.deletebefore(collection, timestamp) — remove vectors older than ts */
int64_t tk_vecstore_deletebefore_w(int64_t coll, int64_t timestamp) {
    if (!coll) return 0;
    TkVecCollection *vc = (TkVecCollection *)(intptr_t)coll;
    return (int64_t)vecstore_delete_before(vc, (int64_t)timestamp);
}
