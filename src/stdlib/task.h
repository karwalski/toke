#ifndef TK_STDLIB_TASK_H
#define TK_STDLIB_TASK_H

/*
 * task.h -- C interface for the std.task standard library module.
 *
 * Structured concurrency primitives: scope (nursery), spawn, await,
 * result retrieval, and cancellation.  All handles are opaque i64
 * values matching the toke compiler's uniform ABI.
 *
 * Implementation uses pthreads with a fixed-size thread pool.
 *
 * Story: 76.1.1a
 */

#include <stdint.h>

/* task.scope():i64 -- create a new task scope (nursery), return handle */
int64_t tk_task_scope(void);

/* task.spawn(scope:i64; fn:i64):i64 -- spawn fn in scope, return task handle */
int64_t tk_task_spawn(int64_t scope, int64_t fn);

/* task.awaitall(scope:i64):i64 -- block until all tasks in scope complete; 0=ok */
int64_t tk_task_awaitall(int64_t scope);

/* task.result(handle:i64):i64 -- get result of completed task */
int64_t tk_task_result(int64_t handle);

/* task.cancel(scope:i64):i64 -- cancel all tasks in scope; 0=ok */
int64_t tk_task_cancel(int64_t scope);

#endif /* TK_STDLIB_TASK_H */
