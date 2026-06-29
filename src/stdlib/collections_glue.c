/*
 * collections_glue.c — i64-ABI wrappers for std.stack, std.queue, std.set,
 *                      and array/map/sort runtime functions.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports collection modules.
 */

#include "collections.h"
#include "tk_array.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * tk_array_retain — bump the refcount on an array-typed handle (ADR-0006 D2).
 * Codegen emits a call to this at every array-typed handle duplication so that
 * a later in-place mutation can tell whether the array is uniquely referenced.
 * Monotonic (never decremented) and NULL-safe.
 */
void tk_array_retain(int64_t h) {
    if (h) ((int64_t *)(intptr_t)h)[-3] += 1;
}

/* ── Map runtime (tk_map_new / tk_map_put / tk_map_get) ────────────── */

typedef struct { int64_t key; int64_t val; } TkMapEntry;
typedef struct { TkMapEntry *entries; int len; int cap; } TkMapImpl;

void *tk_map_new(void) {
    TkMapImpl *m = (TkMapImpl *)calloc(1, sizeof(TkMapImpl));
    return m;
}

void tk_map_put(void *m_ptr, int64_t key, int64_t val) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return;
    const char *ks = (const char *)(intptr_t)key;
    for (int i = 0; i < m->len; i++) {
        const char *ki = (const char *)(intptr_t)m->entries[i].key;
        if ((ks && ki && strcmp(ks, ki) == 0) || (!ks && !ki)) {
            m->entries[i].val = val;
            return;
        }
    }
    if (m->len >= m->cap) {
        int nc = m->cap ? m->cap * 2 : 8;
        TkMapEntry *ne = (TkMapEntry *)realloc(m->entries, (size_t)nc * sizeof(TkMapEntry));
        if (!ne) return;
        m->entries = ne; m->cap = nc;
    }
    m->entries[m->len].key = key;
    m->entries[m->len].val = val;
    m->len++;
}

int64_t tk_map_get(void *m_ptr, int64_t key) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return 0;
    const char *ks = (const char *)(intptr_t)key;
    for (int i = 0; i < m->len; i++) {
        const char *ki = (const char *)(intptr_t)m->entries[i].key;
        if ((ks && ki && strcmp(ks, ki) == 0) || (!ks && !ki))
            return m->entries[i].val;
    }
    return 0;
}

/* ── Array/map instance method wrappers ──────────────────────────────── */

int64_t tk_array_append_w(int64_t arr_i64, int64_t elem) {
    int64_t len = tk_arr_len(arr_i64);
    int64_t h = tk_arr_alloc(len + 1, len + 1);
    if (!h) return arr_i64;
    int64_t *out = (int64_t *)(intptr_t)h;
    if (len > 0) memcpy(out, (int64_t *)(intptr_t)arr_i64, (size_t)len * sizeof(int64_t));
    out[len] = elem;
    return h;
}

int64_t tk_map_set_w(int64_t map_i64, int64_t key, int64_t val) {
    tk_map_put((void *)(intptr_t)map_i64, key, val);
    return map_i64;
}

/* tk_map_keys_w — extract keys from TkMapImpl into a toke-format array. */
int64_t tk_map_keys_w(int64_t map) {
    if (!map) return 0;
    TkMapImpl *m = (TkMapImpl *)(intptr_t)map;
    int64_t count = m->len;
    int64_t h = tk_arr_alloc(count, count);
    if (!h) return 0;
    int64_t *out = (int64_t *)(intptr_t)h;
    for (int i = 0; i < m->len; i++)
        out[i] = m->entries[i].key;
    return h;
}
int64_t tk_map_getor_w(int64_t map, int64_t key, int64_t def) { (void)map; (void)key; return def; }
int64_t tk_map_put_w(int64_t map, int64_t key, int64_t val) {
    if (map) tk_map_put((void *)(intptr_t)map, key, val);
    return 0;
}
int64_t tk_map_getint_w(int64_t map, int64_t key) {
    if (!map) return 0;
    return tk_map_get((void *)(intptr_t)map, key);
}
int64_t tk_map_setint_w(int64_t map, int64_t key, int64_t val) {
    return tk_map_put_w(map, key, val);
}

/* array extras — allocate empty toke-format arrays */
int64_t tk_array_newarray_w(int64_t dummy) {
    (void)dummy;
    return tk_arr_alloc(0, 0);
}
int64_t tk_array_newstrarray_w(int64_t dummy) {
    (void)dummy;
    return tk_arr_alloc(0, 0);
}
int64_t tk_array_strarrayappend_w(int64_t arr, int64_t s) { return tk_array_append_w(arr, s); }
int64_t tk_array_arrayappend_w(int64_t arr, int64_t elem) { return tk_array_append_w(arr, elem); }
int64_t tk_array_appendarray_w(int64_t arr, int64_t arr2) { (void)arr2; return arr; }
int64_t tk_array_list_w(int64_t arr) { return arr; }

/* ── Higher-order array functions (map/filter/reduce/sort) ───────────── */

int64_t tk_arr_map(int64_t arr_i64, int64_t fn_ptr) {
    typedef int64_t (*map_fn)(int64_t);
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    int64_t h = tk_arr_alloc(len, len);
    if (!h) return arr_i64;
    int64_t *out = (int64_t *)(intptr_t)h;
    map_fn f = (map_fn)(intptr_t)fn_ptr;
    for (int64_t i = 0; i < len; i++)
        out[i] = f(ptr[i]);
    return h;
}

int64_t tk_arr_filter(int64_t arr_i64, int64_t fn_ptr) {
    typedef int64_t (*filter_fn)(int64_t);
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    int64_t h = tk_arr_alloc(len, len);
    if (!h) return arr_i64;
    int64_t *out = (int64_t *)(intptr_t)h;
    filter_fn f = (filter_fn)(intptr_t)fn_ptr;
    int64_t out_len = 0;
    for (int64_t i = 0; i < len; i++) {
        if (f(ptr[i]))
            out[out_len] = ptr[i], out_len++;
    }
    tk_arr_setlen(h, out_len);
    return h;
}

int64_t tk_arr_reduce(int64_t arr_i64, int64_t init, int64_t fn_ptr) {
    typedef int64_t (*reduce_fn)(int64_t, int64_t);
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    reduce_fn f = (reduce_fn)(intptr_t)fn_ptr;
    int64_t acc = init;
    for (int64_t i = 0; i < len; i++)
        acc = f(acc, ptr[i]);
    return acc;
}

typedef int64_t (*tk_arr_cmp_fn_t)(int64_t, int64_t);
static tk_arr_cmp_fn_t tk_arr_sort_cmp_global;
static int tk_arr_sort_qsort_cmp(const void *a, const void *b) {
    int64_t va = *(const int64_t *)a;
    int64_t vb = *(const int64_t *)b;
    int64_t r = tk_arr_sort_cmp_global(va, vb);
    return (r > 0) - (r < 0);
}

int64_t tk_arr_sort(int64_t arr_i64, int64_t cmp_ptr) {
    int64_t *ptr = (int64_t *)(intptr_t)arr_i64;
    int64_t len = ptr[-1];
    int64_t h = tk_arr_alloc(len, len);
    if (!h) return arr_i64;
    int64_t *out = (int64_t *)(intptr_t)h;
    if (len > 0) memcpy(out, ptr, (size_t)len * sizeof(int64_t));
    tk_arr_sort_cmp_global = (tk_arr_cmp_fn_t)(intptr_t)cmp_ptr;
    qsort(out, (size_t)len, sizeof(int64_t), tk_arr_sort_qsort_cmp);
    return h;
}

/* ── std.array instance methods ──────────────────────────────────────── */

/* array.get(arr, idx) — get element at index */
int64_t tk_array_get_w(int64_t arr, int64_t idx) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (idx < 0 || idx >= len) return 0;
    return ptr[idx];
}

/* array.length(arr) — get array length */
int64_t tk_array_length_w(int64_t arr) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    return ptr[-1];
}

/* array.new(type) — create empty array (type tag ignored at runtime) */
int64_t tk_array_new_w(int64_t type_tag) {
    (void)type_tag;
    return tk_arr_alloc(0, 0);
}

/* array.push(arr, elem) — append element, return new array */
int64_t tk_array_push_w(int64_t arr, int64_t elem) {
    return tk_array_append_w(arr, elem);
}

/* array.pop(arr) — remove last element, return new array */
int64_t tk_array_pop_w(int64_t arr) {
    if (!arr) return 0;
    int64_t *ptr = (int64_t *)(intptr_t)arr;
    int64_t len = ptr[-1];
    if (len <= 0) return arr;
    int64_t new_len = len - 1;
    int64_t h = tk_arr_alloc(new_len, new_len);
    if (!h) return arr;
    int64_t *out = (int64_t *)(intptr_t)h;
    if (new_len > 0) memcpy(out, ptr, (size_t)new_len * sizeof(int64_t));
    return h;
}

/* array.len — alias for array.length */
int64_t tk_array_len_w(int64_t arr) { return tk_array_length_w(arr); }

/* sort stubs */
int64_t tk_sort_ints_w(int64_t arr) { (void)arr; return arr; }
int64_t tk_sort_strs_w(int64_t arr) { (void)arr; return arr; }

/* ── collections wrappers (toke array layout: block[-1]=count, return block+1) */

/* tk_collections_newarray_w — allocate an empty toke-format array. */
int64_t tk_collections_newarray_w(void) {
    return tk_arr_alloc(0, 0);
}

/* tk_collections_append_w — copy array + append item (immutable style). */
int64_t tk_collections_append_w(int64_t arr, int64_t item) {
    return tk_array_append_w(arr, item);
}

/* tk_collections_push_w — same as append (alias). */
int64_t tk_collections_push_w(int64_t arr, int64_t item) {
    return tk_array_append_w(arr, item);
}

/* ── stack / queue / set wrappers ────────────────────────────────────── */

int64_t tk_stack_new_w(void)                         { return tk_stack_new(); }
int64_t tk_stack_push_w(int64_t s, int64_t val)      { return tk_stack_push(s, val); }
int64_t tk_stack_pop_w(int64_t s)                    { return tk_stack_pop(s); }
int64_t tk_stack_peek_w(int64_t s)                   { return tk_stack_peek(s); }
int64_t tk_stack_len_w(int64_t s)                    { return tk_stack_len(s); }
int64_t tk_stack_empty_w(int64_t s)                  { return tk_stack_empty(s); }

int64_t tk_queue_new_w(void)                         { return tk_queue_new(); }
int64_t tk_queue_push_w(int64_t q, int64_t val)      { return tk_queue_push(q, val); }
int64_t tk_queue_pop_w(int64_t q)                    { return tk_queue_pop(q); }
int64_t tk_queue_peek_w(int64_t q)                   { return tk_queue_peek(q); }
int64_t tk_queue_len_w(int64_t q)                    { return tk_queue_len(q); }

int64_t tk_set_new_w(void)                           { return tk_set_new(); }
int64_t tk_set_add_w(int64_t s, int64_t val)         { return tk_set_add(s, val); }
int64_t tk_set_has_w(int64_t s, int64_t val)         { return tk_set_has(s, val); }
int64_t tk_set_remove_w(int64_t s, int64_t val)      { return tk_set_remove(s, val); }
int64_t tk_set_len_w(int64_t s)                      { return tk_set_len(s); }

int64_t tk_arr_push_w(int64_t arr, int64_t item) {
    return tk_array_append_w(arr, item);
}

/* arr.join — alias for str.join (join array elements with separator) */
int64_t tk_arr_join_w(int64_t arr, int64_t sep) {
    extern int64_t tk_str_join_w(int64_t, int64_t);
    return tk_str_join_w(sep, arr); /* str_join takes (sep, arr) */
}

/* ── std.vec — mutable growable vector wrappers (Story 114.18) ──── */
int64_t tk_vec_new_w(void)                                { return tk_vec_new(); }
int64_t tk_vec_push_w(int64_t v, int64_t val)             { return tk_vec_push(v, val); }
int64_t tk_vec_pop_w(int64_t v)                           { return tk_vec_pop(v); }
int64_t tk_vec_get_w(int64_t v, int64_t idx)              { return tk_vec_get(v, idx); }
int64_t tk_vec_set_w(int64_t v, int64_t idx, int64_t val) { return tk_vec_set(v, idx, val); }
int64_t tk_vec_len_w(int64_t v)                           { return tk_vec_len(v); }
int64_t tk_vec_tovec_w(int64_t arr)                       { return tk_vec_tovec(arr); }
int64_t tk_vec_toarray_w(int64_t v)                       { return tk_vec_toarray(v); }
