#include <stdlib.h>
#include <string.h>

#include "arena.h"
#include "tkc_limits.h"

/*
 * arena.c — bump allocator for the toke reference compiler.
 *
 * Memory is allocated in TKC_TKC_ARENA_BLOCK_SIZE-byte blocks.  When a block is
 * exhausted a new one is prepended to the chain.  arena_free() frees every
 * block and the Arena struct itself.  malloc/free are only used here; no
 * other compiler file may call them directly.
 */

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

/*
 * block_new — Allocate a new ArenaBlock with at least min_cap bytes.
 *
 * Uses TKC_ARENA_BLOCK_SIZE as the default capacity, but grows to
 * min_cap if the requested allocation is larger than a standard block.
 * Returns NULL on malloc failure.
 */
static ArenaBlock *
block_new(int min_cap)
{
    int cap = TKC_ARENA_BLOCK_SIZE;
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

/*
 * arena_init — Create a new Arena and allocate its first block.
 *
 * Returns a heap-allocated Arena with one ready-to-use block, or NULL
 * if memory allocation fails.  The caller must eventually call
 * arena_free() to release all blocks and the Arena struct itself.
 */
Arena *
arena_init(void)
{
    Arena *a = malloc(sizeof(Arena));
    if (!a) return NULL;
    a->head = block_new(TKC_ARENA_BLOCK_SIZE);
    if (!a->head) { free(a); return NULL; }
    return a;
}

/*
 * arena_alloc — Bump-allocate size bytes from the arena.
 *
 * Rounds size up to TKC_ARENA_ALIGN boundary.  If the current head
 * block has insufficient room, a new block is prepended to the chain.
 * The returned memory is zero-initialised.  Returns NULL only if
 * malloc fails when allocating a new block.
 */
void *
arena_alloc(Arena *arena, int size)
{
    if (size <= 0) size = 1;
    /* round up to alignment boundary */
    int aligned = (size + TKC_ARENA_ALIGN - 1) & ~(TKC_ARENA_ALIGN - 1);

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

/*
 * arena_free — Release all blocks in the arena and the Arena struct.
 *
 * Walks the block chain, freeing each block's data buffer and the
 * block header, then frees the Arena itself.  After this call the
 * arena pointer is invalid.
 */
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
