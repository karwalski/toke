/*
 * collections.c -- std.stack, std.queue, std.set standard library modules.
 *
 * Built-in container types backed by simple dynamic arrays.
 * All values are i64 (opaque handles internally).
 *
 * $stack — LIFO via dynamic array (push/pop at tail).
 * $queue — FIFO via circular buffer (enqueue at tail, dequeue from head).
 * $set   — Unique elements via sorted array with binary search.
 *
 * Story: 76.1.7b
 */

#include "collections.h"

#include <stdlib.h>
#include <string.h>
#include <stdint.h>

/* ── Common: dynamic i64 array ───────────────────────────────────── */

#define INIT_CAP 16

typedef struct {
    int64_t *data;
    int64_t  len;
    int64_t  cap;
} DynArr;

static DynArr *dynarr_new(void) {
    DynArr *a = (DynArr *)malloc(sizeof(DynArr));
    if (!a) return NULL;
    a->data = (int64_t *)malloc(INIT_CAP * sizeof(int64_t));
    if (!a->data) { free(a); return NULL; }
    a->len = 0;
    a->cap = INIT_CAP;
    return a;
}

static void dynarr_ensure(DynArr *a, int64_t need) {
    if (a->cap >= need) return;
    int64_t nc = a->cap;
    while (nc < need) nc *= 2;
    int64_t *nd = (int64_t *)realloc(a->data, (size_t)nc * sizeof(int64_t));
    if (!nd) return;
    a->data = nd;
    a->cap = nc;
}

static void dynarr_push(DynArr *a, int64_t val) {
    dynarr_ensure(a, a->len + 1);
    a->data[a->len++] = val;
}

/* ── $stack — LIFO ───────────────────────────────────────────────── */

int64_t tk_stack_new(void) {
    DynArr *s = dynarr_new();
    return (int64_t)(intptr_t)s;
}

int64_t tk_stack_push(int64_t sh, int64_t val) {
    DynArr *s = (DynArr *)(intptr_t)sh;
    if (!s) return sh;
    dynarr_push(s, val);
    return sh;
}

int64_t tk_stack_pop(int64_t sh) {
    DynArr *s = (DynArr *)(intptr_t)sh;
    if (!s || s->len == 0) return 0;
    return s->data[--s->len];
}

int64_t tk_stack_peek(int64_t sh) {
    DynArr *s = (DynArr *)(intptr_t)sh;
    if (!s || s->len == 0) return 0;
    return s->data[s->len - 1];
}

int64_t tk_stack_len(int64_t sh) {
    DynArr *s = (DynArr *)(intptr_t)sh;
    if (!s) return 0;
    return s->len;
}

int64_t tk_stack_empty(int64_t sh) {
    DynArr *s = (DynArr *)(intptr_t)sh;
    if (!s) return 1;
    return s->len == 0 ? 1 : 0;
}

/* ── $queue — FIFO (circular buffer) ─────────────────────────────── */

typedef struct {
    int64_t *data;
    int64_t  cap;
    int64_t  head;   /* index of front element */
    int64_t  len;    /* number of elements */
} Queue;

static Queue *queue_alloc(void) {
    Queue *q = (Queue *)malloc(sizeof(Queue));
    if (!q) return NULL;
    q->data = (int64_t *)malloc(INIT_CAP * sizeof(int64_t));
    if (!q->data) { free(q); return NULL; }
    q->cap = INIT_CAP;
    q->head = 0;
    q->len = 0;
    return q;
}

static void queue_grow(Queue *q) {
    int64_t nc = q->cap * 2;
    int64_t *nd = (int64_t *)malloc((size_t)nc * sizeof(int64_t));
    if (!nd) return;
    /* Linearise the circular buffer into the new allocation */
    for (int64_t i = 0; i < q->len; i++)
        nd[i] = q->data[(q->head + i) % q->cap];
    free(q->data);
    q->data = nd;
    q->cap = nc;
    q->head = 0;
}

int64_t tk_queue_new(void) {
    Queue *q = queue_alloc();
    return (int64_t)(intptr_t)q;
}

int64_t tk_queue_push(int64_t qh, int64_t val) {
    Queue *q = (Queue *)(intptr_t)qh;
    if (!q) return qh;
    if (q->len == q->cap) queue_grow(q);
    int64_t tail = (q->head + q->len) % q->cap;
    q->data[tail] = val;
    q->len++;
    return qh;
}

int64_t tk_queue_pop(int64_t qh) {
    Queue *q = (Queue *)(intptr_t)qh;
    if (!q || q->len == 0) return 0;
    int64_t val = q->data[q->head];
    q->head = (q->head + 1) % q->cap;
    q->len--;
    return val;
}

int64_t tk_queue_peek(int64_t qh) {
    Queue *q = (Queue *)(intptr_t)qh;
    if (!q || q->len == 0) return 0;
    return q->data[q->head];
}

int64_t tk_queue_len(int64_t qh) {
    Queue *q = (Queue *)(intptr_t)qh;
    if (!q) return 0;
    return q->len;
}

/* ── $set — unique elements (sorted array + binary search) ───────── */

typedef struct {
    int64_t *data;
    int64_t  len;
    int64_t  cap;
} Set;

static Set *set_alloc(void) {
    Set *s = (Set *)malloc(sizeof(Set));
    if (!s) return NULL;
    s->data = (int64_t *)malloc(INIT_CAP * sizeof(int64_t));
    if (!s->data) { free(s); return NULL; }
    s->len = 0;
    s->cap = INIT_CAP;
    return s;
}

/* Binary search: returns index where val is or should be inserted.
 * Sets *found to 1 if val is present. */
static int64_t set_bsearch(Set *s, int64_t val, int *found) {
    int64_t lo = 0, hi = s->len;
    while (lo < hi) {
        int64_t mid = lo + (hi - lo) / 2;
        if (s->data[mid] < val)      lo = mid + 1;
        else if (s->data[mid] > val) hi = mid;
        else { *found = 1; return mid; }
    }
    *found = 0;
    return lo;
}

int64_t tk_set_new(void) {
    Set *s = set_alloc();
    return (int64_t)(intptr_t)s;
}

int64_t tk_set_add(int64_t sh, int64_t val) {
    Set *s = (Set *)(intptr_t)sh;
    if (!s) return sh;
    int found = 0;
    int64_t idx = set_bsearch(s, val, &found);
    if (found) return sh;  /* already present */
    /* Ensure capacity */
    if (s->len == s->cap) {
        int64_t nc = s->cap * 2;
        int64_t *nd = (int64_t *)realloc(s->data, (size_t)nc * sizeof(int64_t));
        if (!nd) return sh;
        s->data = nd;
        s->cap = nc;
    }
    /* Shift elements right to make room */
    if (idx < s->len)
        memmove(s->data + idx + 1, s->data + idx,
                (size_t)(s->len - idx) * sizeof(int64_t));
    s->data[idx] = val;
    s->len++;
    return sh;
}

int64_t tk_set_has(int64_t sh, int64_t val) {
    Set *s = (Set *)(intptr_t)sh;
    if (!s) return 0;
    int found = 0;
    set_bsearch(s, val, &found);
    return found ? 1 : 0;
}

int64_t tk_set_remove(int64_t sh, int64_t val) {
    Set *s = (Set *)(intptr_t)sh;
    if (!s) return sh;
    int found = 0;
    int64_t idx = set_bsearch(s, val, &found);
    if (!found) return sh;
    /* Shift elements left */
    if (idx < s->len - 1)
        memmove(s->data + idx, s->data + idx + 1,
                (size_t)(s->len - idx - 1) * sizeof(int64_t));
    s->len--;
    return sh;
}

int64_t tk_set_len(int64_t sh) {
    Set *s = (Set *)(intptr_t)sh;
    if (!s) return 0;
    return s->len;
}
