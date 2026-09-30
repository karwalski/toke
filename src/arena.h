#ifndef TK_ARENA_H
#define TK_ARENA_H

#include <stddef.h>

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
void *arena_alloc(Arena *arena, size_t size);
#endif

/* Create a new arena.  Returns NULL on allocation failure. */
Arena *arena_init(void);

/*
 * Allocate `size` bytes from the arena, aligned to 8 bytes.
 * The returned memory is zero-initialised.
 * Returns NULL on allocation failure, and for a size too large for a block
 * (more than INT_MAX once rounded up) (121.29 REL-06, 127.258).  Callers
 * compute sizes in size_t: `(size_t)n * sizeof(T)`, never `n * (int)sizeof(T)`.
 */
void *arena_alloc(Arena *arena, size_t size);

/*
 * The largest size arena_alloc() can ever satisfy (127.264).  A request above
 * it is refused whatever memory is free, so a caller can reject such a size
 * up front, before any allocation -- `--check` sizes the codegen registries
 * against it so that the check and the build refuse the same caps.
 */
size_t arena_max_alloc(void);

/* Free the arena and all memory allocated from it. */
void arena_free(Arena *arena);

#endif /* TK_ARENA_H */
