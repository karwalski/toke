/* SPDX-License-Identifier: MIT */
/*
 * task.c -- std.task standard library module.
 *
 * Structured concurrency using pthreads.  Provides:
 *   - scope (nursery) creation / destruction
 *   - spawn: submit a toke function value to run on a worker thread
 *   - awaitall: join every task in a scope
 *   - result: retrieve the i64 return value of a completed task
 *   - cancel: request cancellation of all tasks in a scope
 *
 * Thread pool: fixed size = number of online CPUs (sysconf).
 *
 * Story: 76.1.1a
 *
 * 127.549: compiled code spawns through tk_task_spawnfv(scope, fn, fv): `fn`
 * is a C-ABI adapter the compiler emits per signature (@__tk_fvc_<key>, the
 * 127.548 machinery, here with no arguments), `fv` the function value.  The
 * task used to be the bare value called as `int64_t (*)(void)`: a bool task's
 * result was an i1 register whose upper bits x86-64 leaves undefined, an f64
 * task's result sat in a float register C never read, and a closure (env+1)
 * would have been called as code.  The adapter returns a defined i64 (zext of
 * a bool, the bits of an f64, a narrow int extended by its signedness).
 *
 * A closure outlives the spawning statement and runs on another thread, so
 * the task OWNS one reference to the value: the compiler hands it over +1,
 * and the worker releases it (tk_clos_release, an atomic count) once the
 * function has returned -- or at once when the task never runs (cancelled,
 * scope full or not a scope).  A plain function address is not counted, and
 * releasing it is a no-op.
 */

#include "task.h"
#include "tk_runtime.h"   /* 138.47: the handle registry (weak fallback below) */

#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <unistd.h>

/* ── Limits ───────────────────────────────────────────────────────── */

#define TK_TASK_MAX_PER_SCOPE 256

/* ── Task descriptor ──────────────────────────────────────────────── */

typedef struct tk_task {
    pthread_t       thread;
    int64_t         (*fn)(int64_t); /* 127.549: C-ABI adapter, called as fn(fv); NULL: legacy */
    int64_t         fv;             /* the function value; a closure reference the task owns */
    int64_t         result;
    int             done;           /* 1 once the task function has returned */
    int             cancelled;
} tk_task;

/* ── Scope (nursery) ──────────────────────────────────────────────── */

typedef struct tk_scope {
    tk_task        *tasks[TK_TASK_MAX_PER_SCOPE];
    int             count;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    int             cancel_requested;
} tk_scope;

/* ── Thread-pool (lazy init, one pool for the process) ────────────── */

typedef struct tk_pool_job {
    tk_task              *task;
    struct tk_pool_job   *next;
} tk_pool_job;

typedef struct {
    pthread_t      *workers;
    int             nworkers;
    pthread_mutex_t mu;
    pthread_cond_t  cv;
    tk_pool_job    *head;
    tk_pool_job    *tail;
    int             shutdown;
    int             inited;
} tk_pool;

static tk_pool g_pool;
static pthread_once_t g_pool_once = PTHREAD_ONCE_INIT;

/* Worker thread: pull jobs from queue and execute them */
static void *pool_worker(void *arg)
{
    (void)arg;
    for (;;) {
        pthread_mutex_lock(&g_pool.mu);
        while (!g_pool.head && !g_pool.shutdown)
            pthread_cond_wait(&g_pool.cv, &g_pool.mu);

        if (g_pool.shutdown && !g_pool.head) {
            pthread_mutex_unlock(&g_pool.mu);
            break;
        }

        tk_pool_job *job = g_pool.head;
        g_pool.head = job->next;
        if (!g_pool.head) g_pool.tail = NULL;
        pthread_mutex_unlock(&g_pool.mu);

        /* Execute the task */
        tk_task *t = job->task;
        if (!__atomic_load_n(&t->cancelled, __ATOMIC_ACQUIRE)) {
            t->result = t->fn ? t->fn(t->fv)
                              : ((int64_t (*)(void))(intptr_t)t->fv)();
        } else {
            t->result = -1;
        }
        if (t->fn) tk_clos_release(t->fv);   /* 127.549: the task's reference */
        /* 127.549: publish the result before the flag -- awaitall and
         * task.result read them from another thread */
        __atomic_store_n(&t->done, 1, __ATOMIC_RELEASE);
        free(job);
    }
    return NULL;
}

static void pool_init(void)
{
    memset(&g_pool, 0, sizeof g_pool);
    pthread_mutex_init(&g_pool.mu, NULL);
    pthread_cond_init(&g_pool.cv, NULL);

    long ncpu = sysconf(_SC_NPROCESSORS_ONLN);
    if (ncpu < 1) ncpu = 1;
    if (ncpu > 64) ncpu = 64;

    g_pool.nworkers = (int)ncpu;
    g_pool.workers = (pthread_t *)calloc((size_t)ncpu, sizeof(pthread_t));
    if (!g_pool.workers) { g_pool.nworkers = 0; return; }

    for (int i = 0; i < g_pool.nworkers; i++)
        pthread_create(&g_pool.workers[i], NULL, pool_worker, NULL);

    g_pool.inited = 1;
}

static void pool_submit(tk_task *task)
{
    pthread_once(&g_pool_once, pool_init);

    tk_pool_job *job = (tk_pool_job *)malloc(sizeof(tk_pool_job));
    if (!job) {
        /* 127.549: never queued, so finish it here -- a task left not done
         * hung awaitall forever (and kept its closure) */
        task->result = -1;
        if (task->fn) tk_clos_release(task->fv);
        __atomic_store_n(&task->done, 1, __ATOMIC_RELEASE);
        return;
    }
    job->task = task;
    job->next = NULL;

    pthread_mutex_lock(&g_pool.mu);
    if (g_pool.tail) {
        g_pool.tail->next = job;
    } else {
        g_pool.head = job;
    }
    g_pool.tail = job;
    pthread_cond_signal(&g_pool.cv);
    pthread_mutex_unlock(&g_pool.mu);
}

/* ── Task handle tracking (simple global array for handle -> task*) ── */

#define TK_TASK_MAX_HANDLES 4096

static tk_task   *g_handles[TK_TASK_MAX_HANDLES];
static int        g_handle_count;
static pthread_mutex_t g_handle_mu = PTHREAD_MUTEX_INITIALIZER;

static int64_t handle_register(tk_task *t)
{
    pthread_mutex_lock(&g_handle_mu);
    if (g_handle_count >= TK_TASK_MAX_HANDLES) {
        pthread_mutex_unlock(&g_handle_mu);
        return -1;
    }
    int idx = g_handle_count++;
    g_handles[idx] = t;
    pthread_mutex_unlock(&g_handle_mu);
    return (int64_t)(idx + 1); /* 1-based so 0 can indicate error */
}

static tk_task *handle_lookup(int64_t handle)
{
    int idx = (int)(handle - 1);
    if (idx < 0 || idx >= TK_TASK_MAX_HANDLES) return NULL;
    return g_handles[idx];
}

/* ── 138.47: scopes are looked up, not read ───────────────────────────
 *
 * A scope crosses to toke as an i64 (task.tki declares every parameter
 * `i64`, so no checker rule can tell a scope from any other integer).
 * task.awaitall(5), task.cancel(5) and task.spawn(5;f) cast the integer to a
 * tk_scope and locked its mutex: SIGSEGV.  Every scope tk_task_scope makes is
 * now registered in the runtime's handle registry (tk_runtime.c, 138.45) and
 * each call looks its argument up first; a value that is not a live scope is
 * answered as the null scope is (-1).  Task handles already go through the
 * handle table above (task.result(5) answers -1).
 *
 * tk_runtime.c is linked into every program but not into the tkc binary,
 * whose curated link set includes this file (Makefile STDLIB_SRCS) and never
 * calls it; these weak pass-through definitions let that link resolve, and
 * the runtime's strong definitions replace them in every program. */
__attribute__((weak)) void *tk_handle_adopt(int kind, void *p) { (void)kind; return p; }
__attribute__((weak)) void *tk_handle_addr(int kind, int64_t v) { (void)kind; return (void *)(intptr_t)v; }
/* 127.549: the same arrangement for the closure release a finished task
 * makes; tk_runtime.c's definition (an atomic count) is the one programs use. */
__attribute__((weak)) void tk_clos_release(int64_t v) { (void)v; }
static tk_scope *scope_in(int64_t v) { return (tk_scope *)tk_handle_addr(TK_HK_TASKSCOPE, v); }

/* ── Public API ───────────────────────────────────────────────────── */

/* task.scope():i64 */
int64_t tk_task_scope(void)
{
    /* Ensure pool is initialised */
    pthread_once(&g_pool_once, pool_init);

    tk_scope *s = (tk_scope *)calloc(1, sizeof(tk_scope));
    if (!s) return 0;
    pthread_mutex_init(&s->mu, NULL);
    pthread_cond_init(&s->cv, NULL);
    if (!tk_handle_adopt(TK_HK_TASKSCOPE, s)) {   /* 138.47 */
        pthread_mutex_destroy(&s->mu);
        pthread_cond_destroy(&s->cv);
        free(s);
        return 0;
    }
    return (int64_t)(intptr_t)s;
}

/* Queue a task.  `fn` NULL is the legacy C entry (tk_task_spawn): `fv` is
 * then a C function address called as int64_t (*)(void).  Otherwise `fn` is
 * the compiler's adapter and the task owns one reference to `fv`, released
 * here on every path that does not queue it. */
static int64_t spawn_task(int64_t scope, int64_t (*fn)(int64_t), int64_t fv)
{
    tk_scope *s = scope_in(scope);   /* 138.47 */
    if (!s || !fv) {
        if (fn) tk_clos_release(fv);
        return -1;
    }

    pthread_mutex_lock(&s->mu);
    if (s->count >= TK_TASK_MAX_PER_SCOPE || s->cancel_requested) {
        pthread_mutex_unlock(&s->mu);
        if (fn) tk_clos_release(fv);
        return -1;
    }

    tk_task *t = (tk_task *)calloc(1, sizeof(tk_task));
    if (!t) {
        pthread_mutex_unlock(&s->mu);
        if (fn) tk_clos_release(fv);
        return -1;
    }

    t->fn = fn;
    t->fv = fv;
    t->done = 0;
    t->cancelled = 0;
    t->result = 0;

    s->tasks[s->count++] = t;
    pthread_mutex_unlock(&s->mu);

    int64_t handle = handle_register(t);

    /* Submit to thread pool */
    pool_submit(t);

    return handle;
}

/* task.spawn(scope:i64; fn:i64):i64 as compiled code calls it (127.549):
 * `fn` the C-ABI adapter, `fv` the function value (+1, owned by the task). */
int64_t tk_task_spawnfv(int64_t scope, int64_t fn, int64_t fv)
{
    if (!fn) { tk_clos_release(fv); return -1; }
    return spawn_task(scope, (int64_t (*)(int64_t))(intptr_t)fn, fv);
}

/* Legacy C entry (tk_task_spawn_w): `fn` a C function int64_t (*)(void).
 * Compiled toke code never calls this; it calls tk_task_spawnfv. */
int64_t tk_task_spawn(int64_t scope, int64_t fn)
{
    return spawn_task(scope, NULL, fn);
}

/* task.awaitall(scope:i64):i64 */
int64_t tk_task_awaitall(int64_t scope)
{
    tk_scope *s = scope_in(scope);   /* 138.47 */
    if (!s) return -1;

    /* Spin-wait (with yield) for all tasks to complete.
     * A production implementation would use the condvar, but for
     * Phase 1 simplicity we poll with sched_yield. */
    for (;;) {
        int all_done = 1;
        pthread_mutex_lock(&s->mu);
        for (int i = 0; i < s->count; i++) {
            if (!__atomic_load_n(&s->tasks[i]->done, __ATOMIC_ACQUIRE)) { all_done = 0; break; }
        }
        pthread_mutex_unlock(&s->mu);
        if (all_done) break;
        sched_yield();
    }

    return 0;
}

/* task.result(handle:i64):i64 */
int64_t tk_task_result(int64_t handle)
{
    tk_task *t = handle_lookup(handle);
    if (!t) return -1;
    if (!__atomic_load_n(&t->done, __ATOMIC_ACQUIRE)) return -1;   /* not yet finished */
    return t->result;
}

/* task.cancel(scope:i64):i64 */
int64_t tk_task_cancel(int64_t scope)
{
    tk_scope *s = scope_in(scope);   /* 138.47 */
    if (!s) return -1;

    pthread_mutex_lock(&s->mu);
    s->cancel_requested = 1;
    for (int i = 0; i < s->count; i++)
        __atomic_store_n(&s->tasks[i]->cancelled, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s->mu);

    return 0;
}
