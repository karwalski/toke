/*
 * collections.h -- C interface for std.stack, std.queue, std.set modules.
 *
 * Built-in container types with opaque i64 handles matching the toke
 * compiler's uniform ABI.
 *
 * Story: 76.1.7b
 */

#ifndef TK_STDLIB_COLLECTIONS_H
#define TK_STDLIB_COLLECTIONS_H

#include <stdint.h>

/* ── $stack — LIFO container ─────────────────────────────────────── */

int64_t tk_stack_new(void);
int64_t tk_stack_push(int64_t s, int64_t val);
int64_t tk_stack_pop(int64_t s);
int64_t tk_stack_peek(int64_t s);
int64_t tk_stack_len(int64_t s);
int64_t tk_stack_empty(int64_t s);

/* ── $queue — FIFO container ─────────────────────────────────────── */

int64_t tk_queue_new(void);
int64_t tk_queue_push(int64_t q, int64_t val);
int64_t tk_queue_pop(int64_t q);
int64_t tk_queue_peek(int64_t q);
int64_t tk_queue_len(int64_t q);

/* ── $set — unique-element container ─────────────────────────────── */

int64_t tk_set_new(void);
int64_t tk_set_add(int64_t s, int64_t val);
int64_t tk_set_has(int64_t s, int64_t val);
int64_t tk_set_remove(int64_t s, int64_t val);
int64_t tk_set_len(int64_t s);

#endif /* TK_STDLIB_COLLECTIONS_H */
