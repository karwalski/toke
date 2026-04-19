#ifndef TK_STDLIB_VECSTORE_H
#define TK_STDLIB_VECSTORE_H

/*
 * vecstore.h — C interface for the std.vecstore standard library module.
 *
 * Embedded flat-index vector store with cosine similarity search.
 * Vectors are normalised to unit length on upsert; search is a dot product.
 *
 * Type mappings:
 *   str           = const char *  (null-terminated UTF-8)
 *   @(f32) / float* = const float *
 *   i32           = int32_t
 *   i64           = int64_t
 *   f64           = double
 *   bool          = int  (0 = false, 1 = true)
 *
 * Ownership rules:
 *   - vecstore_open     — caller owns the returned TkVecStore* and must call
 *                         vecstore_close to release it.
 *   - vecstore_collection — caller owns the returned TkVecCollection*.
 *                           The collection's lifetime must not exceed that of
 *                           the parent TkVecStore.
 *   - vecstore_search   — caller owns the returned TkSearchResultArray and
 *                         must call vecstore_free_results to release it.
 *   - All other functions return scalar values; no cleanup required.
 *
 * Thread safety: a per-store pthread_mutex serialises all mutations.
 * Concurrent reads from different stores are safe.
 *
 * No external dependencies beyond libc and pthreads.
 *
 * Story: 72.5.3
 */

#include <stdint.h>

/* -------------------------------------------------------------------------
 * Opaque handle types
 * ------------------------------------------------------------------------- */

typedef struct TkVecStore      TkVecStore;
typedef struct TkVecCollection TkVecCollection;

/* -------------------------------------------------------------------------
 * Result types
 * ------------------------------------------------------------------------- */

/* One result from vecstore_search. */
typedef struct {
    char  *id;       /* heap-allocated, owned by TkSearchResultArray */
    double score;    /* cosine similarity in [-1, 1] */
    char  *payload;  /* heap-allocated, owned by TkSearchResultArray */
} TkSearchResult;

/* Array of search results returned by vecstore_search.
 * Free with vecstore_free_results(). */
typedef struct {
    TkSearchResult *items;
    int32_t         count;
} TkSearchResultArray;

/* -------------------------------------------------------------------------
 * Store lifecycle
 * ------------------------------------------------------------------------- */

/* vecstore.open(data_dir:str) -> VecStore!VecErr
 *
 * Opens (or initialises) a vector store rooted at data_dir.  The directory
 * must already exist.  Returns NULL on failure (e.g. directory not found). */
TkVecStore *vecstore_open(const char *data_dir);

/* vecstore.close(vs:VecStore) -> void
 *
 * Flushes all dirty collections to disk and frees all resources.  Each
 * collection is written to {data_dir}/{name}.vecs.  After this call the
 * TkVecStore pointer and all TkVecCollection pointers derived from it are
 * invalid. */
void vecstore_close(TkVecStore *vs);

/* -------------------------------------------------------------------------
 * Collection operations
 * ------------------------------------------------------------------------- */

/* vecstore.collection(vs:VecStore, name:str) -> VecCollection!VecErr
 *
 * Returns a handle for the named collection.  If a backing file
 * {data_dir}/{name}.vecs exists it is loaded; otherwise an empty collection
 * is created.  Returns NULL on I/O error.
 *
 * The returned pointer is owned by the caller but must not outlive the
 * parent TkVecStore. */
TkVecCollection *vecstore_collection(TkVecStore *vs, const char *name);

/* vecstore.upsert(col, id, embedding, dim, payload) -> bool
 *
 * Inserts or replaces an entry.  The embedding is normalised to unit length.
 * Returns 1 on success, 0 on failure (dim mismatch, zero vector, etc.). */
int vecstore_upsert(TkVecCollection *col,
                    const char      *id,
                    const float     *embedding,
                    int32_t          dim,
                    const char      *payload);

/* vecstore.search(col, query, dim, top_k, min_score) -> [SearchResult]
 *
 * Brute-force cosine similarity search.  Returns up to top_k results with
 * score >= min_score, sorted by descending score.
 * Caller must free the result with vecstore_free_results(). */
TkSearchResultArray vecstore_search(TkVecCollection *col,
                                    const float     *query,
                                    int32_t          dim,
                                    int32_t          top_k,
                                    double           min_score);

/* vecstore.delete(col, id) -> bool
 *
 * Removes the entry with the given id.  Returns 1 if found and removed,
 * 0 if not found. */
int vecstore_delete(TkVecCollection *col, const char *id);

/* vecstore.delete_before(col, before_ts) -> i32
 *
 * Removes all entries with created_at < before_ts (Unix seconds).
 * Returns the number of entries removed. */
int32_t vecstore_delete_before(TkVecCollection *col, int64_t before_ts);

/* vecstore.count(col) -> i32
 *
 * Returns the number of entries in the collection. */
int32_t vecstore_count(TkVecCollection *col);

/* -------------------------------------------------------------------------
 * Memory management
 * ------------------------------------------------------------------------- */

/* Free a TkSearchResultArray returned by vecstore_search. */
void vecstore_free_results(TkSearchResultArray *results);

#endif /* TK_STDLIB_VECSTORE_H */
