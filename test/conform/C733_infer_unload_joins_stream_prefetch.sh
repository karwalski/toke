#!/usr/bin/env bash
# C733_infer_unload_joins_stream_prefetch.sh — tk_infer_unload releases a
# streaming handle's prefetch thread and slots (story 136.75).
#
# THE DEFECT.  infer_stream.h and infer_stream.c both said the prefetch
# thread made by tk_infer_load_streaming "is joined on tk_infer_unload", and
# that tk_infer_stream_cleanup is "called from tk_infer_unload".  Nothing
# called it.  Unloading a streaming handle freed the handle and left behind:
# the prefetch thread (parked on a condition variable for ever), its stream
# state, its slot ring and every prefetched shard buffer, plus a registry
# entry keyed by the freed handle's ADDRESS -- so the next handle malloc put
# at that address inherited a dead load's throughput and, on unload, its
# cleanup.
#
# A second defect sat under it.  infer_stream.c allocated the handle through
# its own copy of struct TkModelHandle, commented "MUST match the one in
# infer.c exactly", which did not (id[64]/mu/loaded/void* against infer.c's
# id[128]/model/ctx/lock).  tk_infer_unload, reading a streaming handle with
# infer.c's layout, passed the `loaded` flag (1) to llama_free_model and
# locked a mutex that lay past the end of the allocation.  Both TUs now take
# the one definition from src/stdlib/infer_handle.h.
#
# HOW IT IS OBSERVED.  The streaming path exists only with TK_HAVE_LLAMACPP,
# and no build here has llama.cpp, so the harness supplies a FAKE llama.h and
# a fake implementation of every llama_* that infer.c references.  The fake
# hands out tagged model/context objects and records every free it is given;
# a free of a pointer it never handed out is a BAD FREE (counted, not
# performed).  No model is downloaded; the "model" for the streaming path is
# four small layer_NNN.gguf files of zeros in a temp directory.  Live threads
# are counted from the kernel (task_threads on macOS, /proc/self/task on
# Linux).  Built and run under ASan+UBSan; case 4 again under TSan where the
# host can build it.
#
# WHAT IS ASSERTED (exact values):
#   1-stream-prefetched     load_streaming succeeds and its prefetch thread
#                           loads the first two shards (8192 bytes)
#   1-thread-started        one more live thread than before the load
#   1-unload-returns-1      tk_infer_unload(h) == 1
#   1-thread-joined         live threads back to the pre-load count
#   1-state-gone            tk_infer_stream_cleanup(h) == 0 after unload
#                           (the registry no longer holds the handle; the
#                           call compares the pointer, it does not read it)
#   1-no-bad-free           the fake saw 0 frees of pointers it never made
#   2-plain-unload          a non-streaming load + unload frees exactly one
#                           model and one context, 0 bad frees
#   3-unload-null           tk_infer_unload(NULL) == 0
#   4-cycles                20 load_streaming -> unload cycles end with the
#                           pre-load thread count and every state gone
#   5-asan-clean / 5-ubsan-clean   0 sanitizer reports across the run
#   6-tsan-no-race          0 data races across case 4 (SKIP if the host
#                           cannot build -fsanitize=thread)
#
# Negative control (2026-10-08, INFER_DIR=<src/stdlib at fb0592a1>): the
# harness builds and runs against main's infer.c/infer_stream.c: 8 passed,
# 6 failed -- 1-thread-joined (+1 thread), 1-state-gone (1), 1-no-bad-free
# (1: unload freed the `loaded` flag as a model), 2-plain-unload (the same
# bad free, cumulative), 4-cycles "0 20 21" (20 states left behind) and
# 6-tsan-cycles "1 20 20".  14 passed, 0 failed on the fix.
#
# Story: 136.75

set -uo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../.." && pwd)"
INFER_DIR="${INFER_DIR:-${REPO_ROOT}/src/stdlib}"
CC="${CC:-cc}"
PASS=0; FAIL=0
expect() {
    if [ "$2" = "$3" ]; then echo "PASS $1"; PASS=$((PASS + 1))
    else echo "FAIL $1"; echo "        expected: $3"; echo "        got:      $2"; FAIL=$((FAIL + 1)); fi
}
WORK="$(mktemp -d)"; trap 'rm -rf "${WORK}"' EXIT
mkdir -p "${WORK}/fake" "${WORK}/model"
for i in 000 001 002 003; do
    head -c 4096 /dev/zero > "${WORK}/model/layer_${i}.gguf"
done

# ── the fake llama.cpp API: exactly the names infer.c references ───────────
cat > "${WORK}/fake/llama.h" <<'EOF'
#ifndef FAKE_LLAMA_H
#define FAKE_LLAMA_H
#include <stdint.h>
typedef int32_t llama_token;
struct llama_model;
struct llama_context;
struct llama_model_params   { int32_t n_gpu_layers; };
struct llama_context_params { uint32_t n_threads, n_threads_batch, seed; };
struct llama_batch          { int32_t n_tokens; };
struct llama_model_params   llama_model_default_params(void);
struct llama_context_params llama_context_default_params(void);
struct llama_model   *llama_load_model_from_file(const char *p, struct llama_model_params mp);
struct llama_context *llama_new_context_with_model(struct llama_model *m, struct llama_context_params cp);
void  llama_free(struct llama_context *c);
void  llama_free_model(struct llama_model *m);
int   llama_tokenize(const struct llama_model *m, const char *t, int n, llama_token *out, int max, int bos, int special);
struct llama_batch llama_batch_get_one(llama_token *t, int32_t n, int32_t pos, int32_t seq);
int   llama_decode(struct llama_context *c, struct llama_batch b);
llama_token llama_sample_token_greedy(struct llama_context *c, void *cand);
llama_token llama_token_eos(const struct llama_model *m);
int   llama_token_to_piece(const struct llama_model *m, llama_token t, char *buf, int n, int special);
int   llama_n_embd(const struct llama_model *m);
float *llama_get_embeddings(struct llama_context *c);
#endif
EOF

cat > "${WORK}/harness.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "llama.h"
#include "infer.h"
#include "infer_stream.h"
#if defined(__APPLE__)
#  include <mach/mach.h>
#endif
#include <dirent.h>
#include <pthread.h>

/* ── fake backend ── */
struct llama_model   { int tag; };
struct llama_context { int tag; };
static struct llama_model   g_models[8];   static int g_nmodels;
static struct llama_context g_ctxs[8];     static int g_nctxs;
static int g_free_model, g_free_ctx, g_bad_free;
struct llama_model_params   llama_model_default_params(void)   { struct llama_model_params p = {0}; return p; }
struct llama_context_params llama_context_default_params(void) { struct llama_context_params p = {0, 0, 0}; return p; }
struct llama_model *llama_load_model_from_file(const char *p, struct llama_model_params mp)
{ (void)p; (void)mp; if (g_nmodels >= 8) return NULL; g_models[g_nmodels].tag = 0x4d; return &g_models[g_nmodels++]; }
struct llama_context *llama_new_context_with_model(struct llama_model *m, struct llama_context_params cp)
{ (void)m; (void)cp; if (g_nctxs >= 8) return NULL; g_ctxs[g_nctxs].tag = 0x43; return &g_ctxs[g_nctxs++]; }
void llama_free_model(struct llama_model *m)
{ if (m >= g_models && m < g_models + g_nmodels) g_free_model++; else g_bad_free++; }
void llama_free(struct llama_context *c)
{ if (c >= g_ctxs && c < g_ctxs + g_nctxs) g_free_ctx++; else g_bad_free++; }
int llama_tokenize(const struct llama_model *m, const char *t, int n, llama_token *o, int mx, int b, int s)
{ (void)m; (void)t; (void)n; (void)o; (void)mx; (void)b; (void)s; return -1; }
struct llama_batch llama_batch_get_one(llama_token *t, int32_t n, int32_t p, int32_t q)
{ (void)t; (void)p; (void)q; struct llama_batch b; b.n_tokens = n; return b; }
int llama_decode(struct llama_context *c, struct llama_batch b) { (void)c; (void)b; return -1; }
llama_token llama_sample_token_greedy(struct llama_context *c, void *x) { (void)c; (void)x; return 0; }
llama_token llama_token_eos(const struct llama_model *m) { (void)m; return 0; }
int llama_token_to_piece(const struct llama_model *m, llama_token t, char *b, int n, int s)
{ (void)m; (void)t; (void)b; (void)n; (void)s; return 0; }
int llama_n_embd(const struct llama_model *m) { (void)m; return 0; }
float *llama_get_embeddings(struct llama_context *c) { (void)c; return NULL; }

/* ── live thread count, from the kernel ── */
static int nthreads(void)
{
#if defined(__APPLE__)
    thread_act_array_t list; mach_msg_type_number_t n = 0;
    if (task_threads(mach_task_self(), &list, &n) != KERN_SUCCESS) return -1;
    for (mach_msg_type_number_t i = 0; i < n; i++) mach_port_deallocate(mach_task_self(), list[i]);
    vm_deallocate(mach_task_self(), (vm_address_t)list, n * sizeof(*list));
    return (int)n;
#else
    DIR *d = opendir("/proc/self/task"); struct dirent *e; int n = 0;
    if (!d) return -1;
    while ((e = readdir(d))) if (e->d_name[0] != '.') n++;
    closedir(d); return n;
#endif
}
static void nap(void) { struct timespec ts = {0, 5 * 1000 * 1000}; nanosleep(&ts, NULL); }
/* A joined thread can take a moment to leave the kernel's list. */
static int settle(int want)
{ int n = nthreads(); for (int i = 0; i < 200 && n != want; i++) { nap(); n = nthreads(); } return n; }

/* Sanitizer runtimes start a helper thread of their own on the first
 * pthread_create (TSan does); start and join one throwaway thread before
 * taking the baseline so the count measures only infer's threads. */
static void *noop(void *a) { return a; }
static void warm(void) { pthread_t t; if (!pthread_create(&t, NULL, noop, NULL)) pthread_join(t, NULL); nap(); }

static TkStreamOpts sopts(void) { TkStreamOpts o; o.ram_ceiling_gb = 1.0f; o.prefetch_layers = 2; o.requires_nvme = 0; return o; }

int main(int argc, char **argv)
{
    const char *dir = argv[1];
    const char *mode = argc > 2 ? argv[2] : "all";
    warm();
    int base = settle(nthreads());

    if (!strcmp(mode, "all")) {
        /* 1. load_streaming -> prefetch -> unload */
        TkModelHandleResult r = tk_infer_load_streaming(dir, sopts());
        if (r.is_err || !r.handle) { printf("1-load-err %s\n", r.err.msg ? r.err.msg : "?"); return 1; }
        uint64_t got = 0;
        for (int i = 0; i < 400 && got < 8192; i++) { got = tk_infer_stream_throughput(r.handle).bytes_loaded; if (got < 8192) nap(); }
        printf("1-stream-prefetched %llu\n", (unsigned long long)got);
        printf("1-thread-started %d\n", nthreads() - base);
        uintptr_t addr = (uintptr_t)r.handle;
        printf("1-unload-returns-1 %d\n", tk_infer_unload(r.handle));
        printf("1-thread-joined %d\n", settle(base) - base);
        printf("1-state-gone %d\n", tk_infer_stream_cleanup((TkModelHandle *)addr));
        printf("1-no-bad-free %d\n", g_bad_free);

        /* 2. plain load + unload */
        TkInferOpts io; io.n_gpu_layers = 0; io.n_threads = 0; io.seed = -1;
        TkModelHandleResult p = tk_infer_load("/nonexistent/fake.gguf", io);
        if (p.is_err || !p.handle) { printf("2-load-err %s\n", p.err.msg ? p.err.msg : "?"); return 1; }
        int u = tk_infer_unload(p.handle);
        printf("2-plain-unload %d %d %d %d\n", u, g_free_model, g_free_ctx, g_bad_free);

        /* 3. NULL */
        printf("3-unload-null %d\n", tk_infer_unload(NULL));
    }

    /* 4. cycles */
    int left = 0;
    for (int k = 0; k < 20; k++) {
        TkModelHandleResult r = tk_infer_load_streaming(dir, sopts());
        if (r.is_err || !r.handle) { printf("4-load-err\n"); return 1; }
        if (k % 2) nap();               /* half the time, let prefetch run */
        uintptr_t addr = (uintptr_t)r.handle;
        tk_infer_unload(r.handle);
        left += tk_infer_stream_cleanup((TkModelHandle *)addr);
    }
    printf("4-cycles %d %d %d\n", settle(base) - base, left, g_bad_free);
    return 0;
}
EOF

SAN="-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined -O1 -g"
export ASAN_OPTIONS="detect_leaks=0:abort_on_error=0:detect_stack_use_after_return=1"
export UBSAN_OPTIONS="print_stacktrace=1:halt_on_error=1"
SRCS=("${INFER_DIR}/infer.c" "${INFER_DIR}/infer_stream.c")
CFL=(-std=c99 -D_GNU_SOURCE -DTK_HAVE_LLAMACPP -Wall -Wno-unused-function -iquote "${INFER_DIR}" -I "${WORK}/fake")

if ! "${CC}" "${CFL[@]}" ${SAN} -o "${WORK}/h" "${WORK}/harness.c" "${SRCS[@]}" -lpthread >"${WORK}/cc.log" 2>&1; then
    echo "FAIL build"; sed -n 1,30p "${WORK}/cc.log"; exit 1
fi
out="$("${WORK}/h" "${WORK}/model" all 2>&1)"; rc=$?
field() { printf '%s\n' "${out}" | awk -v k="$1" '$1 == k { $1 = ""; sub(/^ /, ""); print; exit }'; }

expect "1-stream-prefetched" "$(field 1-stream-prefetched)" "8192"
expect "1-thread-started"    "$(field 1-thread-started)"    "1"
expect "1-unload-returns-1"  "$(field 1-unload-returns-1)"  "1"
expect "1-thread-joined"     "$(field 1-thread-joined)"     "0"
expect "1-state-gone"        "$(field 1-state-gone)"        "0"
expect "1-no-bad-free"       "$(field 1-no-bad-free)"       "0"
# unload rc, models freed, contexts freed, bad frees (cumulative)
expect "2-plain-unload"      "$(field 2-plain-unload)"      "1 1 1 0"
expect "3-unload-null"       "$(field 3-unload-null)"       "0"
# threads left over, states left over, bad frees
expect "4-cycles"            "$(field 4-cycles)"            "0 0 0"
expect "5-exit-0"            "${rc}" "0"
expect "5-asan-clean"  "$(printf '%s\n' "${out}" | grep -c 'ERROR: AddressSanitizer')" "0"
expect "5-ubsan-clean" "$(printf '%s\n' "${out}" | grep -c 'runtime error:')" "0"
if [ "${rc}" -ne 0 ] || [ "${FAIL}" -ne 0 ]; then printf '%s\n' "${out}" | sed -n 1,40p | sed 's/^/    | /'; fi

# 6. the cycles under ThreadSanitizer
if "${CC}" "${CFL[@]}" -g -O1 -fsanitize=thread -o "${WORK}/t" "${WORK}/harness.c" "${SRCS[@]}" -lpthread >"${WORK}/tcc.log" 2>&1; then
    tout="$(TSAN_OPTIONS=halt_on_error=0 "${WORK}/t" "${WORK}/model" cycles 2>&1)"
    expect "6-tsan-no-race" "$(printf '%s\n' "${tout}" | grep -c 'ThreadSanitizer: data race')" "0"
    expect "6-tsan-cycles"  "$(printf '%s\n' "${tout}" | awk '$1 == "4-cycles" { print $2, $3, $4 }')" "0 0 0"
else
    echo "SKIP 6: this host cannot build with -fsanitize=thread"; sed -n 1,5p "${WORK}/tcc.log"
fi

echo "C733: ${PASS} passed, ${FAIL} failed"
[ "${FAIL}" -eq 0 ]
