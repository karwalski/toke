/* SPDX-License-Identifier: MIT */
/*
 * vecstore_glue.c — i64-ABI wrappers for std.vecstore module.
 *
 * Vector store for semantic search. Backed by a local flat-index with
 * cosine similarity.
 */

#include "tk_strhdr.h"   /* 127.295: tk_strlen */
#include "vecstore.h"
#include "capabilities.h"   /* 136.15 x 121.22: flush/close take fs.write */
#include "tk_array.h"   /* 114.18: array backing-block header + helpers */
#include "tk_runtime.h" /* 138.61: the handle registry */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 138.61: every store and collection this glue hands out is registered in
 * the runtime's handle registry, by address (a $vecstorehandle's .path and a
 * $veccollection's .name are read from the object itself), and every entry
 * reads only a registered one: a forged `5 as $vecstorehandle`, a record
 * literal or a closed store is answered as the null handle.  A collection
 * is a view of its store, so vecstore.close forgets the store's collections
 * with it (138.64: vecstore_retire keeps their memory, so the addresses are
 * never reused). */
static TkVecStore *store_in(int64_t v)
{
    return (TkVecStore *)tk_handle_addr(TK_HK_VECSTORE, v);
}
static TkVecCollection *coll_in(int64_t v)
{
    return (TkVecCollection *)tk_handle_addr(TK_HK_VECCOLL, v);
}

/* vecstore.open(path) — open or create a vector store at path */
int64_t tk_vecstore_open_w(int64_t path) {
    if (!path) return 0;
    TkVecStore *vs = vecstore_open((const char *)(intptr_t)path);
    /* 138.61: when the table is full the store is left open and unreachable
     * (a fresh store has nothing to write, and vecstore_close would write
     * under fs.write, which open does not take). */
    if (vs && !tk_handle_adopt(TK_HK_VECSTORE, vs)) return 0;
    return (int64_t)(intptr_t)vs;
}

/* vecstore.collection(store, name) — get or create a named collection */
int64_t tk_vecstore_collection_w(int64_t store, int64_t name) {
    /* 121.42: opening a collection loads its file from the store's directory
     * when one exists -- fs.read.  Taken on every call, not only when the
     * file is there: asking first would itself probe the file system. */
    TkVecStore *vs = store_in(store);   /* 138.61 */
    {   /* 138.19: the collection's file, {data_dir}/{name}.vecs (a name
         * holding '/' is refused by vecstore_collection). */
        char cpath[4096];
        const char *dd = vs ? vecstore_datadir(vs) : NULL;
        int ok = dd && name && snprintf(cpath, sizeof cpath, "%s/%s.vecs", dd,
                                        (const char *)(intptr_t)name) < (int)sizeof cpath;
        TK_REQUIRE_PATH(TK_CAP_FS_READ, ok ? cpath : NULL);
    }
    if (!vs || !name) return 0;
    TkVecCollection *col = vecstore_collection(vs, (const char *)(intptr_t)name);
    /* 138.61: a view of its store; the store's own collection list keeps it,
     * so a full table leaves it unreachable from toke rather than leaked. */
    if (col && !tk_handle_adopt_view(TK_HK_VECCOLL, col, vs)) return 0;
    return (int64_t)(intptr_t)col;
}

/* vecstore.upsert(collection, id, embedding, payload) — insert/update vector.
 * The dimension is read from the toke array header rather than passed: toke
 * arrays carry their length, so the C core's explicit `dim` parameter has no
 * toke-level counterpart. The .tki declared a 5th `dim` argument until 136.4
 * because it was transcribed from vecstore.h instead of from this ABI. */
int64_t tk_vecstore_upsert_w(int64_t coll, int64_t id, int64_t embedding, int64_t metadata) {
    TkVecCollection *vc = coll_in(coll);   /* 138.61 */
    if (!vc || !id) return 0;
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

/* vecstore.close(store) — flush every dirty collection to disk and release
 * the store. 136.4: this wrapper never existed, and vecstore_close() is the
 * ONLY caller of collection_save(), so until now a toke consumer had no
 * reachable path to disk at all — upserts accumulated in memory and died with
 * the process. The persistence layer itself (vecstore.c collection_save /
 * collection_load, the "TKVC" format) was complete the whole time; only this
 * entry point was missing. 138.61/138.64: after this call the store handle
 * and every collection handle derived from it are the null handle. */
int64_t tk_vecstore_close_w(int64_t store) {
    TkVecStore *vs = store_in(store);   /* 138.61: a second close is a no-op */
    if (!vs) return 0;
    /* 121.22: close writes only when something is dirty, so only then does it
     * take fs.write -- a read-only consumer closes without the grant. */
    if (vecstore_dirty(vs))
        TK_REQUIRE_PATH(TK_CAP_FS_WRITE, vecstore_datadir(vs));   /* 138.19 */
    (void)tk_handle_drop(vs);   /* 138.61: and its collections */
    /* 138.64: not vecstore_close -- the shells stay allocated, so a later
     * vecstore.open or vecstore.collection never gets this address back and
     * a closed handle stays the null handle. */
    vecstore_retire(vs);
    return 0;
}

/* vecstore.flush(store) — 136.15: write every dirty collection to disk now and
 * keep the store open.  Owner decision 2026-09-22: durability is an explicit
 * call, never a threshold or a timer, so a caller can see when its data is
 * safe.  Returns true only when every dirty collection reached disk. */
int64_t tk_vecstore_flush_w(int64_t store) {
    TkVecStore *vs = store_in(store);   /* 138.61 */
    if (!vs) return 0;
    /* writing is the call's whole purpose; 138.19: into the store's directory */
    TK_REQUIRE_PATH(TK_CAP_FS_WRITE, vecstore_datadir(vs));
    return (int64_t)vecstore_flush(vs);
}

/* vecstore.count(collection) — number of entries currently in the collection */
int64_t tk_vecstore_count_w(int64_t coll) {
    TkVecCollection *vc = coll_in(coll);   /* 138.61 */
    if (!vc) return 0;
    return (int64_t)vecstore_count(vc);
}

/* vecstore.search(collection, query_vec, k, min_score) — k nearest neighbours.
 *
 * Returns @(SearchResult): a toke array whose elements are pointers to 3-slot
 * i64 struct blocks laid out { id:str, score:f64-as-bits, payload:str }, which
 * is how the compiler materialises a 3-field struct (see llvm.c: every field
 * occupies one i64 slot and an f64 is stored bitcast).
 *
 * 136.4: this previously returned a bare array of id strings while the .tki
 * declared [SearchResult]. That combination compiled and then read the first
 * eight bytes of the id text as a pointer, so `results.get(0).id` died with
 * SIGBUS — silent corruption rather than a diagnostic. It also discarded score
 * and payload, which are the only reason to run a similarity search, and
 * leaked the payload strings and the result array because it never called
 * vecstore_free_results. */
int64_t tk_vecstore_search_w(int64_t coll, int64_t query_vec, int64_t k, int64_t min_score_bits) {
    TkVecCollection *vc = coll_in(coll);   /* 138.61 */
    if (!vc || !query_vec || k <= 0) return tk_arr_alloc(0, 0);

    /* f64 crosses the _w ABI as its i64 bit pattern (the json/math/canvas
     * convention), so reinterpret rather than convert. */
    double min_score;
    memcpy(&min_score, &min_score_bits, sizeof min_score);

    /* Decode the query: a toke f64 array, length in handle[-1]. */
    int64_t *ptr = (int64_t *)(intptr_t)query_vec;
    int32_t dim = (int32_t)ptr[-1];
    if (dim < 1) return tk_arr_alloc(0, 0);
    float *vec = (float *)malloc((size_t)dim * sizeof(float));
    if (!vec) return tk_arr_alloc(0, 0);
    for (int32_t i = 0; i < dim; i++) {
        double d;
        memcpy(&d, &ptr[i], sizeof d);
        vec[i] = (float)d;
    }

    TkSearchResultArray results = vecstore_search(vc, vec, dim, (int32_t)k, min_score);
    free(vec);
    if (results.count <= 0) { vecstore_free_results(&results); return tk_arr_alloc(0, 0); }

    int64_t h = tk_arr_alloc((int64_t)results.count, (int64_t)results.count);
    if (!h) { vecstore_free_results(&results); return tk_arr_alloc(0, 0); }
    int64_t *block = (int64_t *)(intptr_t)h;

    int32_t built = 0;
    for (int32_t i = 0; i < results.count; i++) {
        int64_t *sr = (int64_t *)malloc(3 * sizeof(int64_t));
        if (!sr) break;
        /* Copy the strings out: vecstore_free_results owns the originals. */
        const char *src_id  = results.items[i].id      ? results.items[i].id      : "";
        const char *src_pay = results.items[i].payload  ? results.items[i].payload : "";
        /* 127.295: sealed (runtime-abi §3.1) -- toke owns them and reads
         * .len in O(1); a sealed string is never freed, so a failed second
         * copy leaves the first one to the heap like any toke string. */
        char *id  = tks_strdup(src_id);
        char *pay = tks_strdup(src_pay);
        if (!id || !pay) { free(sr); break; }
        int64_t score_bits;
        memcpy(&score_bits, &results.items[i].score, sizeof score_bits);
        sr[0] = (int64_t)(intptr_t)id;   /* .id      */
        sr[1] = score_bits;              /* .score   */
        sr[2] = (int64_t)(intptr_t)pay;  /* .payload */
        block[built++] = (int64_t)(intptr_t)sr;
    }
    tk_arr_setlen(h, (int64_t)built);
    vecstore_free_results(&results);
    return h;
}

/* vecstore.delete(collection, id) — remove a vector by id */
int64_t tk_vecstore_delete_w(int64_t coll, int64_t id) {
    TkVecCollection *vc = coll_in(coll);   /* 138.61 */
    if (!vc || !id) return 0;
    return (int64_t)vecstore_delete(vc, (const char *)(intptr_t)id);
}

/* vecstore.deletebefore(collection, timestamp) — remove vectors older than ts */
int64_t tk_vecstore_deletebefore_w(int64_t coll, int64_t timestamp) {
    TkVecCollection *vc = coll_in(coll);   /* 138.61 */
    if (!vc) return 0;
    return (int64_t)vecstore_delete_before(vc, (int64_t)timestamp);
}
