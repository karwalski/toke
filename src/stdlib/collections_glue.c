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
#include <stdio.h>
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
/*
 * 127.22: open-addressing hash map over an insertion-ordered dense entry
 * array (the "compact dict" layout). Replaces the 2.8.0 linear strcmp list,
 * which made m.get/m.set O(N) and a build loop O(N^2).
 *
 *   entries[0..len)  dense {key,val} pairs in insertion order. keys() and
 *                    template_glue.c (which mirrors the first three fields as
 *                    TkMapImplT) iterate this exactly as they did on 2.8.0, so
 *                    keys() order is unchanged: insertion order, overwrite
 *                    keeps the original position.
 *   hashes[i]        64-bit hash of entries[i].key. A parallel array, kept out
 *                    of TkMapEntry so the mirrored 16-byte entry stays intact.
 *   slots[]          power-of-two index table of int32 indices into entries
 *                    (-1 = empty); linear probing; load factor <= 0.7; doubled
 *                    and rebuilt from entries/hashes on growth. The ABI has no
 *                    remove op, so there are no tombstones.
 *
 * The first three fields (entries, len, cap) are a layout contract shared with
 * template_glue.c and tk_map_len_w — do not reorder them.
 *
 * Key kinds (127.20): codegen passes every key as a bare i64, so the glue can
 * only tell str keys from int keys by a per-map tag fixed at creation:
 *   TK_MAP_KEY_STR  (tk_map_new)      key is a NUL-terminated char*, compared
 *                                     by content, hashed with FNV-1a 64.
 *   TK_MAP_KEY_INT  (tk_map_new_int)  key is the i64 itself, hashed with the
 *                                     splitmix64 finaliser.
 * On a str-keyed map a key that cannot be a user-space pointer (negative, or
 * inside the unmapped zero page) traps as RT006 instead of being handed to
 * strcmp — that was the 2.8.0 `@(1:0); m=m.set(2;5)` segfault.
 *
 * Value semantics are unchanged from 2.8.0: tk_map_put mutates in place and
 * tk_map_set_w returns the same handle (maps have no copy-on-write yet; the
 * language's `m=m.set(k;v)` reassignment idiom is what makes this safe).
 */
#define TK_MAP_KEY_STR   0
#define TK_MAP_KEY_INT   1
#define TK_MAP_MIN_SLOTS 16
#define TK_MAP_MIN_CAP   8

typedef struct { int64_t key; int64_t val; } TkMapEntry;
typedef struct {
    TkMapEntry *entries;   /* dense, insertion order  (mirrored by template_glue.c) */
    int len;               /* entry count             (mirrored) */
    int cap;               /* entries/hashes capacity (mirrored) */
    int kind;              /* TK_MAP_KEY_STR / TK_MAP_KEY_INT */
    int64_t nslots;        /* slots[] length, power of two (0 until first put) */
    int32_t *slots;        /* index into entries, or -1 when empty */
    uint64_t *hashes;      /* hashes[i] == hash of entries[i].key */
} TkMapImpl;

static uint64_t tk_map_hash_str(const char *s) {
    uint64_t h = 1469598103934665603ULL;               /* FNV-1a 64 */
    if (!s) return h;
    for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
    return h;
}
static uint64_t tk_map_hash_int(int64_t k) {
    uint64_t z = (uint64_t)k + 0x9e3779b97f4a7c15ULL; /* splitmix64 finaliser */
    z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
    z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
    return z ^ (z >> 31);
}
static uint64_t tk_map_hash(const TkMapImpl *m, int64_t key) {
    return m->kind == TK_MAP_KEY_INT ? tk_map_hash_int(key)
                                     : tk_map_hash_str((const char *)(intptr_t)key);
}
static int tk_map_key_eq(const TkMapImpl *m, int64_t a, int64_t b) {
    if (m->kind == TK_MAP_KEY_INT) return a == b;
    const char *sa = (const char *)(intptr_t)a, *sb = (const char *)(intptr_t)b;
    if (!sa || !sb) return sa == sb;                   /* NULL key == NULL key, as on 2.8.0 */
    return strcmp(sa, sb) == 0;
}

/* RT006 (127.20): an int used as the key of a str-keyed map. A negative value
 * or one inside the zero page can never be a str pointer, so trap with a
 * message rather than crash inside strcmp. Larger ints are indistinguishable
 * from pointers at this ABI — the complete fix is codegen creating int-keyed
 * map literals via tk_map_new_int (see the 127.20 report / llvm.c patch). */
static void tk_map_check_key(const TkMapImpl *m, int64_t key) {
    if (m->kind == TK_MAP_KEY_STR && (key < 0 || (key > 0 && key < 4096))) {
        fprintf(stderr, "RT006: map key %lld is not a str (int key on a str-keyed map)\n",
                (long long)key);
        exit(1);
    }
}

/* Index into entries of `key` (with hash h), or -1. Load <= 0.7 guarantees
 * an empty slot, so the probe loop always terminates. */
static int tk_map_find(const TkMapImpl *m, int64_t key, uint64_t h) {
    if (m->nslots == 0) return -1;
    uint64_t mask = (uint64_t)m->nslots - 1;
    for (uint64_t i = h & mask;; i = (i + 1) & mask) {
        int32_t e = m->slots[i];
        if (e < 0) return -1;
        if (m->hashes[e] == h && tk_map_key_eq(m, m->entries[e].key, key)) return e;
    }
}

static void tk_map_slot_insert(TkMapImpl *m, int e) {
    uint64_t mask = (uint64_t)m->nslots - 1;
    uint64_t i = m->hashes[e] & mask;
    while (m->slots[i] >= 0) i = (i + 1) & mask;
    m->slots[i] = (int32_t)e;
}

/* Rebuild slots[] at `nslots` (a power of two) from entries/hashes. */
static int tk_map_rehash(TkMapImpl *m, int64_t nslots) {
    int32_t *ns = (int32_t *)malloc((size_t)nslots * sizeof(int32_t));
    if (!ns) return 0;
    memset(ns, 0xff, (size_t)nslots * sizeof(int32_t));   /* every slot = -1 */
    free(m->slots);
    m->slots = ns; m->nslots = nslots;
    for (int e = 0; e < m->len; e++) tk_map_slot_insert(m, e);
    return 1;
}

static void *tk_map_new_kind(int kind) {
    TkMapImpl *m = (TkMapImpl *)calloc(1, sizeof(TkMapImpl));
    if (m) m->kind = kind;
    return m;
}
void *tk_map_new(void)     { return tk_map_new_kind(TK_MAP_KEY_STR); }
/* 127.20: constructor for int-keyed maps — for codegen to emit at a map
 * literal whose key type is i64 (instead of tk_map_new). */
void *tk_map_new_int(void) { return tk_map_new_kind(TK_MAP_KEY_INT); }

void tk_map_put(void *m_ptr, int64_t key, int64_t val) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return;
    tk_map_check_key(m, key);
    uint64_t h = tk_map_hash(m, key);
    int e = tk_map_find(m, key, h);
    if (e >= 0) { m->entries[e].val = val; return; }   /* overwrite keeps position */
    if (m->len >= m->cap) {
        int nc = m->cap ? m->cap * 2 : TK_MAP_MIN_CAP;
        TkMapEntry *ne = (TkMapEntry *)realloc(m->entries, (size_t)nc * sizeof(TkMapEntry));
        if (!ne) return;
        m->entries = ne;
        uint64_t *nh = (uint64_t *)realloc(m->hashes, (size_t)nc * sizeof(uint64_t));
        if (!nh) return;
        m->hashes = nh; m->cap = nc;
    }
    /* keep load <= 0.7 after this insert: (len+1)/nslots <= 7/10 */
    if ((int64_t)(m->len + 1) * 10 > m->nslots * 7) {
        int64_t ns = m->nslots ? m->nslots * 2 : TK_MAP_MIN_SLOTS;
        if (!tk_map_rehash(m, ns)) return;
    }
    e = m->len;
    m->entries[e].key = key;
    m->entries[e].val = val;
    m->hashes[e] = h;
    m->len++;
    tk_map_slot_insert(m, e);
}

int64_t tk_map_get(void *m_ptr, int64_t key) {
    TkMapImpl *m = (TkMapImpl *)m_ptr;
    if (!m) return 0;
    tk_map_check_key(m, key);
    int e = tk_map_find(m, key, tk_map_hash(m, key));
    return e >= 0 ? m->entries[e].val : 0;             /* missing -> 0, as on 2.8.0 */
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

/*
 * tk_array_append_inplace_w — append assuming the caller uniquely owns `arr`
 * (ADR-0006 D2). Emitted by codegen only at a self-update `x = x.append(e)`
 * where a per-function linearity analysis proves `x` is never aliased. Writes
 * into spare capacity (amortised O(1)); doubles capacity and copies once when
 * full. Turns an O(N) loop of appends from O(N^2) into O(N).
 */
int64_t tk_array_append_inplace_w(int64_t arr_i64, int64_t elem) {
    if (!arr_i64) {
        int64_t h = tk_arr_alloc(1, 1);
        if (h) ((int64_t *)(intptr_t)h)[0] = elem;
        return h;
    }
    int64_t *p = (int64_t *)(intptr_t)arr_i64;
    int64_t len = p[-1], cap = p[-2];
    if (len < cap) {            /* spare capacity → write in place */
        p[len] = elem;
        p[-1] = len + 1;
        return arr_i64;
    }
    int64_t ncap = cap > 0 ? cap * 2 : 1;   /* grow (amortised doubling) */
    int64_t h = tk_arr_alloc(ncap, len + 1);
    if (!h) return arr_i64;
    int64_t *out = (int64_t *)(intptr_t)h;
    if (len > 0) memcpy(out, p, (size_t)len * sizeof(int64_t));
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
/* 127.8: map.len — entry count of a TkMapImpl. A map is not an array block,
 * so the ptr[-1] header load the backend used for `.len` read garbage (0). */
int64_t tk_map_len_w(int64_t map) {
    if (!map) return 0;
    return ((TkMapImpl *)(intptr_t)map)->len;
}
/* 127.34: map.getor(k; d) — the stored value when k is present, else d.
 * Unlike tk_map_get (missing -> 0) this distinguishes "absent" from "stored 0",
 * which is what `coll-lookup-default` needs. Same O(1) hash lookup as get. */
int64_t tk_map_getor_w(int64_t map, int64_t key, int64_t def) {
    TkMapImpl *m = (TkMapImpl *)(intptr_t)map;
    if (!m) return def;
    tk_map_check_key(m, key);
    int e = tk_map_find(m, key, tk_map_hash(m, key));
    return e >= 0 ? m->entries[e].val : def;
}
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
