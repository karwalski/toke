/*
 * task.c -- std.task standard library module.
 *
 * Structured concurrency using pthreads.  Provides:
 *   - scope (nursery) creation / destruction
 *   - spawn: submit a toke function pointer to run on a worker thread
 *   - awaitall: join every task in a scope
 *   - result: retrieve the i64 return value of a completed task
 *   - cancel: request cancellation of all tasks in a scope
 *
 * Thread pool: fixed size = number of online CPUs (sysconf).
 *
 * Story: 76.1.1a
 */

#include "task.h"

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
    int64_t         (*fn)(void);    /* toke function pointer (no args, returns i64) */
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
        if (!t->cancelled) {
            t->result = t->fn();
        } else {
            t->result = -1;
        }
        t->done = 1;
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
    if (!job) return;
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
    return (int64_t)(intptr_t)s;
}

/* task.spawn(scope:i64; fn:i64):i64 */
int64_t tk_task_spawn(int64_t scope, int64_t fn)
{
    tk_scope *s = (tk_scope *)(intptr_t)scope;
    if (!s || !fn) return -1;

    pthread_mutex_lock(&s->mu);
    if (s->count >= TK_TASK_MAX_PER_SCOPE || s->cancel_requested) {
        pthread_mutex_unlock(&s->mu);
        return -1;
    }

    tk_task *t = (tk_task *)calloc(1, sizeof(tk_task));
    if (!t) { pthread_mutex_unlock(&s->mu); return -1; }

    t->fn = (int64_t(*)(void))(intptr_t)fn;
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

/* task.awaitall(scope:i64):i64 */
int64_t tk_task_awaitall(int64_t scope)
{
    tk_scope *s = (tk_scope *)(intptr_t)scope;
    if (!s) return -1;

    /* Spin-wait (with yield) for all tasks to complete.
     * A production implementation would use the condvar, but for
     * Phase 1 simplicity we poll with sched_yield. */
    for (;;) {
        int all_done = 1;
        pthread_mutex_lock(&s->mu);
        for (int i = 0; i < s->count; i++) {
            if (!s->tasks[i]->done) { all_done = 0; break; }
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
    if (!t->done) return -1;   /* not yet finished */
    return t->result;
}

/* task.cancel(scope:i64):i64 */
int64_t tk_task_cancel(int64_t scope)
{
    tk_scope *s = (tk_scope *)(intptr_t)scope;
    if (!s) return -1;

    pthread_mutex_lock(&s->mu);
    s->cancel_requested = 1;
    for (int i = 0; i < s->count; i++)
        s->tasks[i]->cancelled = 1;
    pthread_mutex_unlock(&s->mu);

    return 0;
}
