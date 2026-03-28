#ifndef TK_ARENA_H
#define TK_ARENA_H

/*
 * arena.h — opaque bump allocator used throughout the toke reference compiler.
 *
 * All AST nodes, scope entries, and interned strings are arena-allocated.
 * arena_free() reclaims the entire allocation in one operation at end-of-
 * compilation.  No individual free() is needed or supported.
 *
 * Arena * is an opaque pointer; callers must not dereference the struct.
 */

#ifndef TK_ARENA_TYPES_DEFINED
#define TK_ARENA_TYPES_DEFINED
typedef struct Arena Arena;
void *arena_alloc(Arena *arena, int size);
#endif

/* Create a new arena.  Returns NULL on allocation failure. */
Arena *arena_init(void);

/*
 * Allocate `size` bytes from the arena, aligned to 8 bytes.
 * The returned memory is zero-initialised.
 * Returns NULL on allocation failure.
 */
void *arena_alloc(Arena *arena, int size);

/* Free the arena and all memory allocated from it. */
void arena_free(Arena *arena);

#endif /* TK_ARENA_H */
