#include <limits.h>
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
 * arena_max_alloc — the largest request arena_alloc() accepts: INT_MAX less
 * the alignment slack, so the rounded-up size still fits an int (127.264).
 */
size_t
arena_max_alloc(void)
{
    return (size_t)INT_MAX - (TKC_ARENA_ALIGN - 1);
}

/*
 * arena_alloc — Bump-allocate size bytes from the arena.
 *
 * Rounds size up to TKC_ARENA_ALIGN boundary.  If the current head
 * block has insufficient room, a new block is prepended to the chain.
 * The returned memory is zero-initialised.  Returns NULL if malloc fails
 * when allocating a new block, or if size is too large for a block (more
 * than INT_MAX once rounded up) (121.29, 127.258).
 */
void *
arena_alloc(Arena *arena, size_t size)
{
    /* 121.29 (REL-06) / 127.258: the size is a size_t so a caller's product
     * reaches this check whole.  With an `int` parameter a size_t product
     * above INT_MAX was truncated at the call, and an `n * (int)sizeof(T)`
     * overflowed in the caller: `--max-funcs=1000000` asked for 2.6 GB of
     * FnSig, got a negative int (NULL, then a NULL dereference), and
     * `--max-funcs=2000000` wrapped to a positive ~0.9 GB block the caller
     * then indexed as if it held two million entries.  Callers now multiply
     * in size_t, and anything a block cannot hold -- including a negative
     * int converted to size_t -- is refused with NULL, the same answer as an
     * allocation failure, which callers handle. */
    if (size == 0) size = 1;
    if (size > arena_max_alloc()) return NULL;
    /* round up to alignment boundary */
    int aligned = ((int)size + TKC_ARENA_ALIGN - 1) & ~(TKC_ARENA_ALIGN - 1);

    ArenaBlock *b = arena->head;
    if (aligned > b->cap - b->used) {   /* not used + aligned: no overflow */
        ArenaBlock *nb = block_new(aligned);
        if (!nb) return NULL;
        nb->next    = arena->head;
        arena->head = nb;
        b = nb;
    }

    void *p = (void *)(b->data + b->used);
    b->used += aligned;
    memset(p, 0, size);
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
