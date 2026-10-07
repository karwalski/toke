/*
 * test_strhdr.c — the sealed string heap (src/stdlib/tk_strhdr.h, story 127.170).
 *
 * Build and run: make test-stdlib-strhdr   (also built with -DTK_STR_NO_HEAP,
 * where every case must hold with no header anywhere).
 *
 * What is asserted, by value:
 *   1. threads FIRST, before anything else touches the heap: 8 threads race
 *      the one-time reservation, then allocate, seal and cross-read each
 *      other's strings; every trusted length equals strlen and every byte is
 *      the one its producer wrote.  Run under ASan and TSan by hand (the
 *      report says which).
 *   2. a sealed string's header is trusted; a literal, a malloc'd string, an
 *      unsealed heap block and an interior pointer are not;
 *   3. a string with a 0 byte seals with the C-string length;
 *   4. allocations past the first 64 MiB commit chunk stay sealed and intact;
 *   5. the verify switch (child processes, so an abort is observable):
 *      a planted write into a sealed string gives the silent wrong answer
 *      with TK_STR_VERIFY unset, and aborts with TK_STR_VERIFY=1;
 *   6. free() of a sealed string does not return normally.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>
#include "../../src/stdlib/tk_strhdr.h"

static int failures = 0;

#define ASSERT(cond, msg) \
    do { if (!(cond)) { fprintf(stderr, "FAIL: %s\n", msg); failures++; } \
         else { printf("pass: %s\n", msg); } } while (0)

/* Expected header for a sealed string: its length with the heap, -1 without. */
static int64_t want_hdr(size_t n) { return TKS_HEAP ? (int64_t)n : -1; }

/* ── 1. threads ─────────────────────────────────────────────────────────── */
#define NTHREADS 8
#define PER_THREAD 20000
static char *g_strs[NTHREADS][PER_THREAD];
static int   g_ready;           /* start gate */
static int   g_produced[NTHREADS];
static int   g_bad[NTHREADS];

static void *worker(void *arg)
{
    int id = (int)(intptr_t)arg;
    while (!__atomic_load_n(&g_ready, __ATOMIC_ACQUIRE)) { /* spin */ }
    char buf[300];
    for (int i = 0; i < PER_THREAD; i++) {
        size_t n = (size_t)((i * 7 + id * 13) % 257);
        for (size_t k = 0; k < n; k++) buf[k] = (char)('a' + (int)((k + (size_t)i + (size_t)id) % 26));
        char *p = (i & 1) ? tks_dup(buf, n) : NULL;
        if (!p) {                      /* the alloc + write + seal path */
            p = tks_alloc(n);
            memcpy(p, buf, n);
            p[n] = '\0';
            tks_seal(p, n);
        }
        if (tks_hdrlen(p) != want_hdr(n) || tk_strlen(p) != n || memcmp(p, buf, n) != 0)
            g_bad[id]++;
        g_strs[id][i] = p;
        __atomic_store_n(&g_produced[id], i + 1, __ATOMIC_RELEASE);
        /* read a string another thread has published */
        int other = (id + 1 + i % (NTHREADS - 1)) % NTHREADS;
        int have = __atomic_load_n(&g_produced[other], __ATOMIC_ACQUIRE);
        if (have > 0) {
            const char *q = g_strs[other][i % have];
            if (tk_strlen(q) != strlen(q)) g_bad[id]++;
        }
    }
    return NULL;
}

static void test_threads(void)
{
    pthread_t t[NTHREADS];
    for (int i = 0; i < NTHREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)(intptr_t)i);
    __atomic_store_n(&g_ready, 1, __ATOMIC_RELEASE);
    for (int i = 0; i < NTHREADS; i++) pthread_join(t[i], NULL);
    int bad = 0;
    for (int i = 0; i < NTHREADS; i++) bad += g_bad[i];
    ASSERT(bad == 0, "threads: 8 x 20000 concurrent alloc/seal/cross-read, every length and byte right");
    /* every block distinct and still intact after all threads finished */
    int intact = 1;
    for (int id = 0; id < NTHREADS && intact; id++)
        for (int i = 0; i < PER_THREAD; i++) {
            size_t n = (size_t)((i * 7 + id * 13) % 257);
            const char *p = g_strs[id][i];
            if (strlen(p) != n || tks_hdrlen(p) != want_hdr(n)) { intact = 0; break; }
            for (size_t k = 0; k < n; k++)
                if (p[k] != (char)('a' + (int)((k + (size_t)i + (size_t)id) % 26))) { intact = 0; break; }
        }
    ASSERT(intact, "threads: no block overlaps another (all 160000 intact afterwards)");
}

/* ── 2-4. trust and exclusion ───────────────────────────────────────────── */
static void test_trust(void)
{
    char *s = tks_dup("hello, world", 12);
    ASSERT(s && strcmp(s, "hello, world") == 0, "tks_dup copies the bytes and the NUL");
    ASSERT(tks_hdrlen(s) == want_hdr(12), "sealed: header carries the length");
    ASSERT(tk_strlen(s) == 12, "sealed: tk_strlen == 12");
    if (TKS_HEAP) ASSERT(((uintptr_t)s & 15) == 8, "sealed: pointer is 16-aligned + 8");

    ASSERT(tks_hdrlen("a literal") == -1, "literal: no header trusted");
    ASSERT(tk_strlen("a literal") == 9, "literal: tk_strlen falls back to strlen");
    char *m = malloc(32); strcpy(m, "malloced");
    ASSERT(tks_hdrlen(m) == -1, "malloc'd string: no header trusted");
    free(m);

    char *u = tks_alloc(5); memcpy(u, "abcde", 6);
    ASSERT(tks_hdrlen(u) == -1, "allocated but unsealed: no header trusted");
    ASSERT(tk_strlen(u) == 5, "unsealed: tk_strlen == strlen");
    tks_seal(u, 5);
    ASSERT(tks_hdrlen(u) == want_hdr(5), "sealed after the fact: header trusted");

    char big[200]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = '\0';
    char *b = tks_dup(big, 199);
    ASSERT(tks_hdrlen(b + 16) == -1, "interior pointer at a granule's +8: no bit, no header");
    ASSERT(tks_hdrlen(b + 1) == -1, "interior pointer off the grid: no header");
    ASSERT(tk_strlen(b + 16) == 183, "interior pointer: tk_strlen == strlen");

    char z[6] = {'a', 'b', 0, 'c', 'd', 0};
    char *zp = tks_alloc(5); memcpy(zp, z, 6);
    tks_seal(zp, strlen(zp));
    ASSERT(tk_strlen(zp) == 2, "a 0 byte: sealed with the C-string length");

    char *e = tks_dup("", 0);
    ASSERT(e && e[0] == '\0' && tks_hdrlen(e) == want_hdr(0), "empty string seals with 0");

    char *d = tks_strdup(s);
    ASSERT(d != s && strcmp(d, s) == 0 && tks_hdrlen(d) == want_hdr(12), "tks_strdup");
}

static void test_chunks(void)
{
    /* 80 x 1 MiB: crosses at least one 64 MiB commit boundary */
    size_t n = (size_t)1 << 20;
    char *buf = malloc(n);
    int ok = 1;
    char *first = NULL, *last = NULL;
    for (int i = 0; i < 80 && ok; i++) {
        memset(buf, 'A' + i % 26, n);
        char *p = tks_dup(buf, n);
        if (!p || tk_strlen(p) != n || p[0] != 'A' + i % 26 || p[n - 1] != 'A' + i % 26) ok = 0;
        if (!first) first = p;
        last = p;
    }
    ASSERT(ok, "80 MiB of 1 MiB strings: every one sealed and intact across commit chunks");
    ASSERT(tk_strlen(first) == n && tk_strlen(last) == n, "first and last still carry their length");
    free(buf);
}

/* ── 5-6. the verify switch and the never-freed rule ────────────────────── */
static int run_child(const char *self, const char *mode, const char *verify, char *out, size_t outsz)
{
    int fd[2];
    if (pipe(fd) != 0) return -1;
    pid_t pid = fork();
    if (pid == 0) {
        dup2(fd[1], 1); dup2(fd[1], 2); close(fd[0]); close(fd[1]);
        if (verify) setenv("TK_STR_VERIFY", verify, 1); else unsetenv("TK_STR_VERIFY");
        setenv("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1", 1);
        execl(self, self, mode, (char *)NULL);
        _exit(127);
    }
    close(fd[1]);
    size_t got = 0; ssize_t r;
    while (got + 1 < outsz && (r = read(fd[0], out + got, outsz - 1 - got)) > 0) got += (size_t)r;
    out[got] = '\0';
    close(fd[0]);
    int st = 0;
    waitpid(pid, &st, 0);
    return st;
}

static int child_main(const char *mode)
{
    if (!strcmp(mode, "--plant")) {
        char *s = tks_dup("abcdef", 6);
        s[3] = '\0';                  /* the forbidden write */
        printf("len=%zu text=%s\n", tk_strlen(s), s);
        return 0;
    }
    if (!strcmp(mode, "--free")) {
        char *s = tks_dup("abcdef", 6);
        free(s);                      /* the forbidden free */
        printf("free returned\n");
        return 0;
    }
    return 2;
}

static void test_verify(const char *self)
{
    char out[4096];
    int st = run_child(self, "--plant", NULL, out, sizeof out);
    if (TKS_HEAP) {
        ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0 && strstr(out, "len=6 text=abc"),
               "verify off: a write into a sealed string gives the silent wrong answer (len=6 text=abc)");
        st = run_child(self, "--plant", "1", out, sizeof out);
        ASSERT(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT &&
               strstr(out, "TK_STR_VERIFY: header 6 != strlen 3"),
               "TK_STR_VERIFY=1: the same write aborts with 'header 6 != strlen 3'");
        st = run_child(self, "--free", NULL, out, sizeof out);
        ASSERT(!(WIFEXITED(st) && WEXITSTATUS(st) == 0) && !strstr(out, "free returned"),
               "free() of a sealed string does not return normally");
    } else {
        ASSERT(WIFEXITED(st) && WEXITSTATUS(st) == 0 && strstr(out, "len=3 text=abc"),
               "no heap: strlen always, the write is merely visible (len=3)");
    }
}

int main(int argc, char **argv)
{
    if (argc > 1) return child_main(argv[1]);
    printf("heap: %s\n", TKS_HEAP ? "sealed string heap" : "fallback (TK_STR_NO_HEAP or no mmap)");
    test_threads();           /* first: races the one-time reservation */
    test_trust();
    test_chunks();
    test_verify(argv[0]);
    if (failures == 0) { printf("All tests passed.\n"); return 0; }
    fprintf(stderr, "%d test(s) failed.\n", failures);
    return 1;
}
