/*
 * infer_stream.c — disk-streaming extension for the std.infer module.
 *
 * Implements:
 *   tk_infer_detect_storage_type()  — NVMe/SSD/HDD classification
 *   tk_infer_load_streaming()       — layer-by-layer GGUF shard loading
 *   tk_infer_stream_throughput()    — live throughput metrics query
 *   tk_infer_stream_cleanup()       — release streaming resources on unload
 *
 * Storage detection (tk_infer_detect_storage_type) is unconditional: it does
 * not require llama.cpp and is always compiled.
 *
 * The remaining functions (load_streaming, stream_throughput, stream_cleanup)
 * are gated behind TK_HAVE_LLAMACPP.  Without that macro, they return
 * graceful errors so callers fail at runtime rather than at link time.
 *
 * C99, POSIX threads (pthread), no external dependencies beyond libc.
 *
 * Story: 72.7.1–72.7.5
 */

#include "infer_stream.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <time.h>

/* POSIX: pthread, stat, dirent */
#include <pthread.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

/* Platform-specific storage detection */
#if defined(__APPLE__)
#  include <sys/mount.h>   /* statfs */
#endif

/* =========================================================================
 * Internal constants
 * ========================================================================= */

/* Maximum number of per-layer shard paths we can track. */
#define TK_STREAM_MAX_SHARDS  4096

/* Maximum filesystem path length used internally. */
#define TK_STREAM_PATH_MAX    4096

/* Size of the ring buffer used to compute rolling throughput. */
#define TK_STREAM_TPUT_WINDOW 16

/* =========================================================================
 * Story 72.7.2 — NVMe / storage-type detection
 *
 * This section is unconditional (no TK_HAVE_LLAMACPP guard) because storage
 * detection is a standalone utility that does not depend on llama.cpp.
 * ========================================================================= */

/*
 * On Linux, given a path we:
 *   1. stat() to find the device number.
 *   2. Walk /sys/block to find the block device whose dev number matches.
 *   3. Read queue/rotational (0 = non-spinning).
 *   4. Read device/transport (contains "nvme" for NVMe drives).
 */
#if defined(__linux__)

static const char *detect_linux(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return "unknown";

    /* Walk /sys/block looking for the device. */
    DIR *d = opendir("/sys/block");
    if (!d)
        return "unknown";

    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '.')
            continue;

        /* Read the dev file: major:minor */
        char dev_path[TK_STREAM_PATH_MAX];
        snprintf(dev_path, sizeof(dev_path),
                 "/sys/block/%s/dev", ent->d_name);
        FILE *f = fopen(dev_path, "r");
        if (!f) continue;

        unsigned int maj = 0, min_val = 0;
        int n = fscanf(f, "%u:%u", &maj, &min_val);
        fclose(f);
        if (n != 2) continue;

        dev_t block_dev = makedev(maj, min_val);
        if (block_dev != st.st_dev) {
            /* Also check if st.st_dev falls within this block device
             * (partition): read /sys/block/<dev>/<partition>/dev */
            char part_dir[TK_STREAM_PATH_MAX];
            snprintf(part_dir, sizeof(part_dir),
                     "/sys/block/%s", ent->d_name);
            DIR *pd = opendir(part_dir);
            if (pd) {
                struct dirent *pe;
                int found = 0;
                while ((pe = readdir(pd)) != NULL && !found) {
                    if (pe->d_name[0] == '.') continue;
                    char pdev[TK_STREAM_PATH_MAX];
                    snprintf(pdev, sizeof(pdev),
                             "/sys/block/%s/%s/dev",
                             ent->d_name, pe->d_name);
                    FILE *pf = fopen(pdev, "r");
                    if (!pf) continue;
                    unsigned int pmaj = 0, pmin = 0;
                    if (fscanf(pf, "%u:%u", &pmaj, &pmin) == 2) {
                        if (makedev(pmaj, pmin) == st.st_dev)
                            found = 1;
                    }
                    fclose(pf);
                }
                closedir(pd);
                if (!found) continue;
            } else {
                continue;
            }
        }

        /* Found the block device.  Check transport. */
        char transport_path[TK_STREAM_PATH_MAX];
        snprintf(transport_path, sizeof(transport_path),
                 "/sys/block/%s/device/transport", ent->d_name);
        FILE *tf = fopen(transport_path, "r");
        if (tf) {
            char transport[64] = {0};
            if (fgets(transport, sizeof(transport), tf) != NULL) {
                fclose(tf);
                closedir(d);
                if (strstr(transport, "nvme"))
                    return "nvme";
            } else {
                fclose(tf);
            }
        }

        /* Check rotational flag. */
        char rot_path[TK_STREAM_PATH_MAX];
        snprintf(rot_path, sizeof(rot_path),
                 "/sys/block/%s/queue/rotational", ent->d_name);
        FILE *rf = fopen(rot_path, "r");
        if (rf) {
            int rotational = 1;
            if (fscanf(rf, "%d", &rotational) == 1) {
                fclose(rf);
                closedir(d);
                return rotational ? "hdd" : "ssd";
            }
            fclose(rf);
        }

        closedir(d);
        return "unknown";
    }
    closedir(d);
    return "unknown";
}

#endif /* __linux__ */

/*
 * On macOS, we use `diskutil info <dev>` via popen() to read storage metadata.
 *
 * The authoritative approach is to open the BSD device path via IOKit and
 * read the "Protocol Characteristics" dictionary; the "Physical Interconnect"
 * key will contain "NVMe" for NVMe drives.  Since IOKit requires linking
 * against IOKit.framework (not available in a pure C compilation unit without
 * the framework flag), we use `diskutil info` via popen() as the portable
 * fallback that works in all build environments.
 */
#if defined(__APPLE__)

/* diskutil info output contains lines like:
 *   Solid State:              Yes
 *   Protocol:                 Apple Fabric  (or NVMe, SATA, etc.)
 */
static const char *detect_macos(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0)
        return "unknown";

    /* Find the BSD device node for this file's filesystem. */
    struct statfs sfs;
    if (statfs(path, &sfs) != 0)
        return "unknown";

    /* sfs.f_mntfromname is e.g. "/dev/disk3s1" */
    char cmd[TK_STREAM_PATH_MAX + 64];
    snprintf(cmd, sizeof(cmd), "diskutil info '%s' 2>/dev/null",
             sfs.f_mntfromname);

    FILE *pipe = popen(cmd, "r");
    if (!pipe)
        return "unknown";

    int    is_ssd   = 0;
    int    is_nvme  = 0;
    char   line[256];

    while (fgets(line, sizeof(line), pipe) != NULL) {
        /* "Solid State: Yes" indicates non-spinning media. */
        if (strstr(line, "Solid State") && strstr(line, "Yes"))
            is_ssd = 1;

        /*
         * Protocol line examples:
         *   Protocol:                 NVMe
         *   Protocol:                 Apple Fabric   (also NVMe internally)
         *   Protocol:                 SATA
         */
        if (strstr(line, "Protocol")) {
            if (strstr(line, "NVMe") || strstr(line, "Apple Fabric"))
                is_nvme = 1;
        }
    }
    pclose(pipe);

    if (is_nvme)  return "nvme";
    if (is_ssd)   return "ssd";
    return "hdd";
}

#endif /* __APPLE__ */

/*
 * tk_infer_detect_storage_type — public API, dispatches to platform helpers.
 *
 * Returns one of: "nvme", "ssd", "hdd", "unknown" (static storage duration).
 */
const char *tk_infer_detect_storage_type(const char *path)
{
    if (!path || path[0] == '\0')
        return "unknown";

#if defined(__APPLE__)
    return detect_macos(path);
#elif defined(__linux__)
    return detect_linux(path);
#else
    (void)path;
    return "unknown";
#endif
}

/* =========================================================================
 * The rest of this file is gated behind TK_HAVE_LLAMACPP.
 * Without llama.cpp, stub implementations are provided at the bottom.
 * ========================================================================= */

#ifdef TK_HAVE_LLAMACPP

/* =========================================================================
 * Internal types (TK_HAVE_LLAMACPP build only)
 * ========================================================================= */

/* Prefetch slot: one buffer per prefetched shard. */
typedef struct {
    void    *buf;         /* malloc'd shard bytes; NULL if empty */
    size_t   size;        /* byte length of buf */
    int      ready;       /* 1 when data is fully loaded */
    int      error;       /* 1 if the load failed */
} TkPrefetchSlot;

/*
 * TkStreamState — private streaming state associated with a TkModelHandle.
 *
 * The public TkModelHandle type is opaque (defined in infer.c).  Because this
 * compilation unit cannot reach the internal layout of TkModelHandle, we
 * manage a separate registry that maps handles to their TkStreamState.  A
 * singly-linked list is sufficient — the number of concurrently open
 * streaming handles in practice is tiny.
 */
typedef struct TkStreamState {
    /* Configuration captured at load time. */
    char         model_dir[TK_STREAM_PATH_MAX];
    TkStreamOpts opts;

    /* Shard inventory */
    char         shard_paths[TK_STREAM_MAX_SHARDS][TK_STREAM_PATH_MAX];
    int          n_shards;
    int          current_shard;   /* index of the shard currently in decode */

    /* Prefetch ring: slot_count entries, indexed mod slot_count. */
    TkPrefetchSlot *slots;
    int             slot_count;

    /* RAM accounting (bytes) */
    uint64_t        ram_used;
    uint64_t        ram_ceiling;  /* converted from ram_ceiling_gb at init */

    /* Throughput tracking */
    double   tput_window[TK_STREAM_TPUT_WINDOW]; /* tokens/sec samples */
    int      tput_head;                           /* ring-buffer write head */
    int      tput_count;                          /* number of valid samples */
    uint64_t bytes_loaded;

    /* Prefetch thread */
    pthread_t       prefetch_thread;
    int             thread_running;
    pthread_mutex_t lock;
    pthread_cond_t  cond_slot_free;
    pthread_cond_t  cond_slot_ready;
    int             shutdown;     /* set to 1 to stop the prefetch thread */

    /* Handle this state belongs to (used for registry lookup). */
    const TkModelHandle *owner;

    struct TkStreamState *next;   /* intrusive list linkage */
} TkStreamState;

/* =========================================================================
 * Global registry of active streaming states
 * ========================================================================= */

static pthread_mutex_t  g_registry_lock = PTHREAD_MUTEX_INITIALIZER;
static TkStreamState   *g_registry_head = NULL;

static void registry_insert(TkStreamState *s)
{
    pthread_mutex_lock(&g_registry_lock);
    s->next = g_registry_head;
    g_registry_head = s;
    pthread_mutex_unlock(&g_registry_lock);
}

static TkStreamState *registry_find(const TkModelHandle *h)
{
    pthread_mutex_lock(&g_registry_lock);
    TkStreamState *cur = g_registry_head;
    while (cur) {
        if (cur->owner == h) break;
        cur = cur->next;
    }
    pthread_mutex_unlock(&g_registry_lock);
    return cur;
}

static void registry_remove(const TkModelHandle *h)
{
    pthread_mutex_lock(&g_registry_lock);
    TkStreamState **pp = &g_registry_head;
    while (*pp) {
        if ((*pp)->owner == h) {
            *pp = (*pp)->next;
            pthread_mutex_unlock(&g_registry_lock);
            return;
        }
        pp = &(*pp)->next;
    }
    pthread_mutex_unlock(&g_registry_lock);
}

/* =========================================================================
 * Shard inventory (72.7.3)
 * ========================================================================= */

/*
 * build_shard_list — populate s->shard_paths[] by scanning model_dir for
 * files matching layer_NNN.gguf, inserted in ascending layer-index order.
 *
 * Returns the number of shards found, or -1 on error.
 */
static int build_shard_list(TkStreamState *s)
{
    DIR *d = opendir(s->model_dir);
    if (!d) {
        fprintf(stderr,
                "[infer_stream] cannot open model dir: %s (%s)\n",
                s->model_dir, strerror(errno));
        return -1;
    }

    int count = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && count < TK_STREAM_MAX_SHARDS) {
        unsigned int idx = 0;
        if (sscanf(ent->d_name, "layer_%u.gguf", &idx) != 1)
            continue;

        char full[TK_STREAM_PATH_MAX];
        snprintf(full, sizeof(full), "%s/%s", s->model_dir, ent->d_name);

        /* Insertion sort by layer index. */
        int pos = count;
        while (pos > 0) {
            unsigned int prev_idx = 0;
            const char *bn = strrchr(s->shard_paths[pos - 1], '/');
            bn = bn ? bn + 1 : s->shard_paths[pos - 1];
            sscanf(bn, "layer_%u.gguf", &prev_idx);
            if (prev_idx <= idx) break;
            memcpy(s->shard_paths[pos], s->shard_paths[pos - 1],
                   TK_STREAM_PATH_MAX);
            pos--;
        }
        memcpy(s->shard_paths[pos], full, TK_STREAM_PATH_MAX);
        count++;
    }
    closedir(d);
    return count;
}

/* =========================================================================
 * RAM accounting helpers
 * ========================================================================= */

static uint64_t ram_ceiling_bytes(float gb)
{
    /* 1 GiB = 1073741824 bytes */
    return (uint64_t)((double)gb * 1073741824.0);
}

/* =========================================================================
 * Throughput helpers (72.7.4)
 * ========================================================================= */

/*
 * record_throughput — push a new tokens/sec sample into the rolling window.
 * Must be called with s->lock held.
 */
static void record_throughput(TkStreamState *s, double tok_per_sec)
{
    s->tput_window[s->tput_head] = tok_per_sec;
    s->tput_head = (s->tput_head + 1) % TK_STREAM_TPUT_WINDOW;
    if (s->tput_count < TK_STREAM_TPUT_WINDOW)
        s->tput_count++;

    if (tok_per_sec < TK_STREAM_SLOW_THRESHOLD_TOK_S) {
        fprintf(stderr,
                "[infer_stream] WARNING: throughput %.4f tok/s is below "
                "threshold %.1f tok/s\n",
                tok_per_sec, TK_STREAM_SLOW_THRESHOLD_TOK_S);
    }
}

/* =========================================================================
 * Prefetch thread (72.7.3)
 * ========================================================================= */

/*
 * prefetch_thread_fn — background thread that fills prefetch slots ahead of
 * the decode window.
 *
 * Pipeline:
 *   while not shutdown:
 *     wait until a slot is free and RAM budget allows
 *     read shard into slot buffer
 *     mark slot ready, signal main thread
 */
static void *prefetch_thread_fn(void *arg)
{
    TkStreamState *s = (TkStreamState *)arg;

    int next_to_prefetch = 0;

    while (1) {
        pthread_mutex_lock(&s->lock);

        /* Wait while we are ahead of the decode window or have no work. */
        while (!s->shutdown &&
               (next_to_prefetch >= s->n_shards ||
                next_to_prefetch >= s->current_shard + s->slot_count)) {
            pthread_cond_wait(&s->cond_slot_free, &s->lock);
        }

        if (s->shutdown) {
            pthread_mutex_unlock(&s->lock);
            break;
        }

        int shard_idx = next_to_prefetch;
        int slot_idx  = shard_idx % s->slot_count;
        const char *shard_path = s->shard_paths[shard_idx];
        pthread_mutex_unlock(&s->lock);

        /* Stat shard to get its size before allocating. */
        struct stat st;
        if (stat(shard_path, &st) != 0) {
            pthread_mutex_lock(&s->lock);
            s->slots[slot_idx].buf   = NULL;
            s->slots[slot_idx].size  = 0;
            s->slots[slot_idx].error = 1;
            s->slots[slot_idx].ready = 1;
            pthread_cond_broadcast(&s->cond_slot_ready);
            pthread_mutex_unlock(&s->lock);
            next_to_prefetch++;
            continue;
        }

        size_t shard_size = (size_t)st.st_size;

        /* Wait for RAM budget. */
        pthread_mutex_lock(&s->lock);
        while (!s->shutdown &&
               (s->ram_used + shard_size > s->ram_ceiling)) {
            pthread_cond_wait(&s->cond_slot_free, &s->lock);
        }
        if (s->shutdown) {
            pthread_mutex_unlock(&s->lock);
            break;
        }
        s->ram_used += shard_size;
        pthread_mutex_unlock(&s->lock);

        /* Load shard from disk. */
        void *buf    = malloc(shard_size);
        int   load_ok = 0;
        if (buf) {
            FILE *f = fopen(shard_path, "rb");
            if (f) {
                size_t got = fread(buf, 1, shard_size, f);
                fclose(f);
                if (got == shard_size)
                    load_ok = 1;
            }
        }

        pthread_mutex_lock(&s->lock);
        if (load_ok) {
            s->slots[slot_idx].buf   = buf;
            s->slots[slot_idx].size  = shard_size;
            s->slots[slot_idx].error = 0;
            s->bytes_loaded         += shard_size;
        } else {
            free(buf);
            s->slots[slot_idx].buf   = NULL;
            s->slots[slot_idx].size  = 0;
            s->slots[slot_idx].error = 1;
            /* Refund the RAM charge. */
            s->ram_used -= shard_size;
        }
        s->slots[slot_idx].ready = 1;
        pthread_cond_broadcast(&s->cond_slot_ready);
        pthread_mutex_unlock(&s->lock);

        next_to_prefetch++;
    }

    return NULL;
}

/* =========================================================================
 * Minimal opaque handle definition
 *
 * The real struct lives in infer.c but we need a local definition to allocate
 * one from this translation unit.  This definition MUST match the one in
 * infer.c exactly.
 * ========================================================================= */

struct TkModelHandle {
    char            id[64];
    pthread_mutex_t mu;
    int             loaded;
    void           *llama_ctx;   /* TODO: struct llama_context * */
    void           *llama_model; /* TODO: struct llama_model   * */
};

/* =========================================================================
 * tk_infer_load_streaming (72.7.3)
 * ========================================================================= */

TkModelHandleResult tk_infer_load_streaming(const char  *model_dir,
                                              TkStreamOpts opts)
{
    TkModelHandleResult result = {NULL, 0, {NULL, 0}};

    if (!model_dir || model_dir[0] == '\0') {
        result.is_err   = 1;
        result.err.msg  = "model_dir must not be empty";
        result.err.code = -1;
        return result;
    }

    /* 72.7.2 — detect storage type and warn if required. */
    const char *storage = tk_infer_detect_storage_type(model_dir);
    if (opts.requires_nvme && strcmp(storage, "nvme") != 0) {
        fprintf(stderr,
                "[infer_stream] WARNING: requires_nvme is set but detected "
                "storage type is \"%s\" for path: %s\n",
                storage, model_dir);
    }

    /* Allocate and initialise streaming state. */
    TkStreamState *s = (TkStreamState *)calloc(1, sizeof(TkStreamState));
    if (!s) {
        result.is_err   = 1;
        result.err.msg  = "out of memory allocating stream state";
        result.err.code = -1;
        return result;
    }

    strncpy(s->model_dir, model_dir, TK_STREAM_PATH_MAX - 1);
    s->opts        = opts;
    s->ram_ceiling = ram_ceiling_bytes(opts.ram_ceiling_gb > 0.0f
                                       ? opts.ram_ceiling_gb : 4.0f);
    s->slot_count  = opts.prefetch_layers > 0 ? opts.prefetch_layers : 1;

    /* Build shard inventory. */
    int n = build_shard_list(s);
    if (n <= 0) {
        free(s);
        result.is_err   = 1;
        result.err.msg  = "no layer_NNN.gguf shards found in model_dir";
        result.err.code = -1;
        return result;
    }
    s->n_shards = n;

    /* Allocate prefetch ring. */
    s->slots = (TkPrefetchSlot *)calloc((size_t)s->slot_count,
                                         sizeof(TkPrefetchSlot));
    if (!s->slots) {
        free(s);
        result.is_err   = 1;
        result.err.msg  = "out of memory allocating prefetch ring";
        result.err.code = -1;
        return result;
    }

    pthread_mutex_init(&s->lock, NULL);
    pthread_cond_init(&s->cond_slot_free, NULL);
    pthread_cond_init(&s->cond_slot_ready, NULL);

    /* Allocate the model handle. */
    TkModelHandle *h = (TkModelHandle *)calloc(1, sizeof(TkModelHandle));
    if (!h) {
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->cond_slot_free);
        pthread_cond_destroy(&s->cond_slot_ready);
        free(s->slots);
        free(s);
        result.is_err   = 1;
        result.err.msg  = "out of memory allocating model handle";
        result.err.code = -1;
        return result;
    }

    snprintf(h->id, sizeof(h->id), "stream:%s", model_dir);
    pthread_mutex_init(&h->mu, NULL);
    h->loaded = 1;

    s->owner = h;
    registry_insert(s);

    /* Spawn prefetch thread. */
    s->thread_running = 1;
    if (pthread_create(&s->prefetch_thread, NULL, prefetch_thread_fn, s) != 0) {
        registry_remove(h);
        s->thread_running = 0;
        pthread_mutex_destroy(&s->lock);
        pthread_cond_destroy(&s->cond_slot_free);
        pthread_cond_destroy(&s->cond_slot_ready);
        free(s->slots);
        free(s);
        pthread_mutex_destroy(&h->mu);
        free(h);
        result.is_err   = 1;
        result.err.msg  = "failed to start prefetch thread";
        result.err.code = -1;
        return result;
    }

    /*
     * TODO (TK_HAVE_LLAMACPP full impl): iterate s->n_shards, for each:
     *   - wait on cond_slot_ready for the corresponding slot to be ready
     *   - call llama_model_load_from_file() on the buffered shard
     *   - accumulate layers into the combined llama_context
     *   - free(slot->buf); slot->buf = NULL; ram_used -= slot->size
     *   - signal cond_slot_free so the prefetch thread can refill
     * Track tokens processed and call record_throughput() each iteration.
     */

    result.handle = h;
    return result;
}

/* =========================================================================
 * tk_infer_stream_throughput (72.7.4)
 * ========================================================================= */

TkStreamThroughput tk_infer_stream_throughput(const TkModelHandle *h)
{
    TkStreamThroughput out = {0.0, 0, 0, 0};
    if (!h) return out;

    TkStreamState *s = registry_find(h);
    if (!s) return out;

    pthread_mutex_lock(&s->lock);

    out.bytes_loaded   = s->bytes_loaded;
    out.ram_used_bytes = s->ram_used;

    if (s->tput_count > 0) {
        double sum = 0.0;
        for (int i = 0; i < s->tput_count; i++)
            sum += s->tput_window[i];
        out.tokens_per_sec = sum / (double)s->tput_count;
        out.warn_slow = (out.tokens_per_sec < TK_STREAM_SLOW_THRESHOLD_TOK_S)
                        ? 1 : 0;
    }

    pthread_mutex_unlock(&s->lock);
    return out;
}

/* =========================================================================
 * tk_infer_stream_cleanup — called from tk_infer_unload (72.7.3)
 * ========================================================================= */

/*
 * Shut down the prefetch thread and free all streaming state associated with h.
 * Returns 1 if a stream state was found and cleaned up, 0 otherwise.
 */
int tk_infer_stream_cleanup(TkModelHandle *h)
{
    if (!h) return 0;

    TkStreamState *s = registry_find(h);
    if (!s) return 0;

    pthread_mutex_lock(&s->lock);
    s->shutdown = 1;
    pthread_cond_broadcast(&s->cond_slot_free);
    pthread_cond_broadcast(&s->cond_slot_ready);
    pthread_mutex_unlock(&s->lock);

    if (s->thread_running)
        pthread_join(s->prefetch_thread, NULL);

    for (int i = 0; i < s->slot_count; i++) {
        free(s->slots[i].buf);
        s->slots[i].buf = NULL;
    }

    registry_remove(h);

    pthread_mutex_destroy(&s->lock);
    pthread_cond_destroy(&s->cond_slot_free);
    pthread_cond_destroy(&s->cond_slot_ready);
    free(s->slots);
    free(s);

    return 1;
}

/* Suppress unused-function warning for record_throughput in stub builds. */
static void _unused_record_throughput_ref(TkStreamState *s, double v)
{
    record_throughput(s, v);
}
/* Cast to void to prevent "defined but not used" on the wrapper itself. */
static void (*const _rtp_ref)(TkStreamState *, double) __attribute__((unused))
    = _unused_record_throughput_ref;

#else /* !TK_HAVE_LLAMACPP — stub implementations */

/* =========================================================================
 * Stubs: TK_HAVE_LLAMACPP is not defined.
 *
 * tk_infer_detect_storage_type is defined above (unconditionally).
 * Only load_streaming, stream_throughput, and stream_cleanup need stubs.
 * ========================================================================= */

TkModelHandleResult tk_infer_load_streaming(const char  *model_dir,
                                              TkStreamOpts opts)
{
    (void)model_dir;
    (void)opts;
    TkModelHandleResult r;
    r.handle   = NULL;
    r.is_err   = 1;
    r.err.msg  = "std.infer: llama.cpp not available "
                 "(rebuild with -DTK_HAVE_LLAMACPP)";
    r.err.code = -2;
    return r;
}

TkStreamThroughput tk_infer_stream_throughput(const TkModelHandle *h)
{
    (void)h;
    TkStreamThroughput out;
    out.tokens_per_sec = 0.0;
    out.bytes_loaded   = 0;
    out.ram_used_bytes = 0;
    out.warn_slow      = 0;
    return out;
}

int tk_infer_stream_cleanup(TkModelHandle *h)
{
    (void)h;
    return 0;
}

#endif /* TK_HAVE_LLAMACPP */
