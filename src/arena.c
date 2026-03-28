#include <stdlib.h>
#include <string.h>

#include "arena.h"

/*
 * arena.c — bump allocator for the toke reference compiler.
 *
 * Memory is allocated in 64 KB blocks.  When a block is exhausted a new one
 * is prepended to the chain.  arena_free() frees every block and the Arena
 * struct itself.  malloc/free are only used here; no other compiler file may
 * call them directly.
 */

#define ARENA_BLOCK_SIZE (64 * 1024)
#define ARENA_ALIGN      8

typedef struct ArenaBlock ArenaBlock;
struct ArenaBlock {
    char       *data;
    int         cap;
    int         used;
    ArenaBlock *next;
};

struct Arena {
    ArenaBlock *head;
};

static ArenaBlock *
block_new(int min_cap)
{
    int cap = ARENA_BLOCK_SIZE;
    if (min_cap > cap) cap = min_cap;

    ArenaBlock *b = malloc(sizeof(ArenaBlock));
    if (!b) return NULL;
    b->data = malloc((size_t)cap);
    if (!b->data) { free(b); return NULL; }
    b->cap  = cap;
    b->used = 0;
    b->next = NULL;
    return b;
}

Arena *
arena_init(void)
{
    Arena *a = malloc(sizeof(Arena));
    if (!a) return NULL;
    a->head = block_new(ARENA_BLOCK_SIZE);
    if (!a->head) { free(a); return NULL; }
    return a;
}

void *
arena_alloc(Arena *arena, int size)
{
    if (size <= 0) size = 1;
    /* round up to alignment boundary */
    int aligned = (size + ARENA_ALIGN - 1) & ~(ARENA_ALIGN - 1);

    ArenaBlock *b = arena->head;
    if (b->used + aligned > b->cap) {
        ArenaBlock *nb = block_new(aligned);
        if (!nb) return NULL;
        nb->next    = arena->head;
        arena->head = nb;
        b = nb;
    }

    void *p = (void *)(b->data + b->used);
    b->used += aligned;
    memset(p, 0, (size_t)size);
    return p;
}

void
arena_free(Arena *arena)
{
    ArenaBlock *b = arena->head;
    while (b) {
        ArenaBlock *next = b->next;
        free(b->data);
        free(b);
        b = next;
    }
    free(arena);
}
