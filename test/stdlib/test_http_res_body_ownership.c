/*
 * test_http_res_body_ownership.c — regression test for story 127.148.
 *
 * A `Res` DOES NOT OWN ITS BODY.  http_Res_ok/_json/_bad/_err store the
 * `body` pointer as given; make_res() copies nothing.  That is deliberate —
 * it is what lets a handler return a string literal, and it is what lets
 *
 *     http.resok(http.param(req; "name"))
 *
 * work at all, since the `:param` binding is request-lifetime memory and is
 * released after the response is written (story 127.72).  But the rule was
 * never written down and never enforced, so the handlers that build a body
 * on the HEAP simply dropped it:
 *
 *   staticdir_handler        — malloc's the whole file, plus a StrPair block
 *   tk_page_dispatch         — tmpl_renderpage() returns heap, plus a block
 *   vhost_catchall_handler   — router_static_serve's own contract says the
 *                              caller must free resp.body and the header
 *                              values; no caller did.  On a 200 that is the
 *                              entire file, four strdup'd header values and
 *                              two pointer arrays, every request.  (Removed
 *                              with the unused vhost surface, 136.69.)
 *
 * So the same design choice that makes 127.72's fix correct also made every
 * static file the server ever sent a per-request leak the size of the file.
 *
 * THE RULE, now stated in http.h and docs/stdlib/http.md: a body is either
 * (a) static storage, (b) memory owned by the request, which outlives the
 * response by construction, or (c) heap the handler hands to
 * http_defer_free() in the same breath.  There is no fourth case.
 *
 * WHAT THIS MEASURES.  A real http.servedir mount, driven through the real
 * server loop via http_handle_fd() over a loopback keep-alive connection —
 * the same harness shape as test_http_route_param_leak.c — with the process
 * heap's in-use byte count read before and after.  Heap accounting, not RSS:
 * RSS cannot see this on a running toke server, because the per-connection
 * read buffer is ~1 MB and because a worker killed by SIGPIPE is respawned
 * and carries its leaked memory away with it (see 127.144).
 *
 * The served file is 64 KiB, so before the fix the leak is ~65600 bytes per
 * request — three orders of magnitude above the threshold, and not something
 * allocator noise or machine load can produce or hide.
 *
 * Build and run: test/conform/S008_http_servedir_mounts.sh
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include "../../src/stdlib/http.h"

/* tk_web_glue.c's i64-ABI wrappers are not declared in any header. */
extern int64_t tk_http_serve_staticdir_w(int64_t prefix, int64_t root);
/* 121.42: http.servedir takes fs.read at registration; this harness is the
 * program, so it grants itself read as a compiled program's --allow-read. */
extern void tk_cap_init(int argc, char **argv);

#if defined(__APPLE__)
#  include <malloc/malloc.h>
#  define HAVE_HEAP_STATS 1
static size_t heap_in_use(void)
{
    malloc_statistics_t st;
    memset(&st, 0, sizeof st);
    malloc_zone_statistics(NULL, &st);  /* NULL = all zones */
    return (size_t)st.size_in_use;
}
#elif defined(__linux__) && defined(__GLIBC__)
#  include <malloc.h>
#  define HAVE_HEAP_STATS 1
static size_t heap_in_use(void)
{
    struct mallinfo2 mi = mallinfo2();
    return (size_t)mi.uordblks;
}
#else
#  define HAVE_HEAP_STATS 0
static size_t heap_in_use(void) { return 0; }
#endif

#define ASSET_BYTES       (64 * 1024)
#define WARMUP_REQUESTS   32
#define MEASURE_REQUESTS  200   /* KEEPALIVE_MAX_REQUESTS is 1000 */
#define MEASURE_CONNS     4
/*
 * 64 B/request.  The fixed per-connection and allocator-zone costs that
 * survive amortise to well under this over 800 requests (the 127.72 harness
 * measured the same residual as flat, not per-request), while the defect
 * this pins is ~65600 B/request — a factor of a thousand.  There is no load
 * on any machine that closes that gap in either direction.
 */
#define MAX_BYTES_PER_REQUEST 64

static int  g_listen_fd = -1;
static int  g_port      = 0;
static char g_root[512];

static const char REQ[] =
    "GET /assets/blob.bin HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static void *client_thread(void *arg)
{
    long n = (long)(intptr_t)arg;
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return (void *)(intptr_t)-1;

    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        close(s); return (void *)(intptr_t)-1;
    }

    for (long i = 0; i < n; i++) {
        size_t off = 0, len = sizeof(REQ) - 1;
        while (off < len) {
            ssize_t w = write(s, REQ + off, len - off);
            if (w <= 0) { close(s); return (void *)(intptr_t)-1; }
            off += (size_t)w;
        }
        /* Drain one whole response.  The static handler sets no
         * Content-Length, so the server sends it chunked; read until the
         * terminating zero-length chunk. */
        char   buf[16384];
        size_t have = 0;
        int    done = 0;
        while (!done) {
            ssize_t r = read(s, buf, sizeof(buf));
            if (r <= 0) { close(s); return (void *)(intptr_t)-1; }
            have += (size_t)r;
            if ((size_t)r >= 5 && memcmp(buf + r - 5, "0\r\n\r\n", 5) == 0)
                done = 1;
        }
        if (have < ASSET_BYTES) { close(s); return (void *)(intptr_t)-1; }
    }
    close(s);
    return (void *)(intptr_t)0;
}

static int serve_one(long n)
{
    pthread_t th;
    void *rc = NULL;
    if (pthread_create(&th, NULL, client_thread, (void *)(intptr_t)n) != 0)
        return -1;
    int fd = accept(g_listen_fd, NULL, NULL);
    if (fd < 0) { pthread_join(th, &rc); return -1; }
    http_handle_fd(fd);          /* closes fd itself */
    pthread_join(th, &rc);
    return (int)(intptr_t)rc;
}

int main(void)
{
    alarm(180); /* safety net */

#if !HAVE_HEAP_STATS
    printf("SKIP: no heap-accounting API on this platform\n");
    return 0;
#else
    /* ── lay down the asset under <tmpdir>/assets/blob.bin ── */
    char tmpl[] = "/tmp/tkc_resbody_XXXXXX";
    char *dir = mkdtemp(tmpl);
    if (!dir) { fprintf(stderr, "FAIL: mkdtemp: %s\n", strerror(errno)); return 1; }
    snprintf(g_root, sizeof g_root, "%s", dir);

    char sub[600];
    snprintf(sub, sizeof sub, "%s/assets", g_root);
    if (mkdir(sub, 0700) != 0) {
        fprintf(stderr, "FAIL: mkdir %s: %s\n", sub, strerror(errno));
        return 1;
    }
    char asset[700];
    snprintf(asset, sizeof asset, "%s/blob.bin", sub);
    {
        FILE *f = fopen(asset, "wb");
        if (!f) { fprintf(stderr, "FAIL: fopen %s\n", asset); return 1; }
        for (int i = 0; i < ASSET_BYTES; i++) fputc('a' + (i % 26), f);
        fclose(f);
    }

    /* ── the route under test: a real http.servedir mount ── */
    {
        char *cap_argv[] = { "test_http_res_body_ownership", "--allow-read", NULL };
        tk_cap_init(2, cap_argv);
    }
    if (tk_http_serve_staticdir_w((int64_t)(intptr_t)"/assets",
                                  (int64_t)(intptr_t)g_root) != 0) {
        fprintf(stderr, "FAIL: http.servedir mount was rejected\n");
        return 1;
    }

    /* ── listening socket ── */
    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { fprintf(stderr, "FAIL: socket\n"); return 1; }
    int on = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = 0;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listen_fd, (struct sockaddr *)&a, sizeof a) < 0 ||
        listen(g_listen_fd, 8) < 0 ||
        getsockname(g_listen_fd, (struct sockaddr *)&a, &alen) < 0) {
        fprintf(stderr, "FAIL: could not listen: %s\n", strerror(errno));
        return 1;
    }
    g_port = ntohs(a.sin_port);

    if (serve_one(WARMUP_REQUESTS) != 0) {
        fprintf(stderr, "FAIL: warm-up connection did not complete\n");
        return 1;
    }

    size_t before = heap_in_use();
    for (int k = 0; k < MEASURE_CONNS; k++) {
        if (serve_one(MEASURE_REQUESTS) != 0) {
            fprintf(stderr, "FAIL: measured connection %d did not complete\n", k);
            return 1;
        }
    }
    size_t after = heap_in_use();

    long   delta = (long)after - (long)before;
    int    reqs  = MEASURE_REQUESTS * MEASURE_CONNS;
    double per   = (double)delta / (double)reqs;

    printf("servedir body: heap in use before=%zu after=%zu delta=%ld over %d requests\n",
           before, after, delta, reqs);
    printf("servedir body: %.1f bytes per request (threshold %d, asset is %d bytes)\n",
           per, MAX_BYTES_PER_REQUEST, ASSET_BYTES);

    unlink(asset); rmdir(sub); rmdir(g_root);

    if (per > (double)MAX_BYTES_PER_REQUEST) {
        printf("FAIL: the response body is still being dropped on the floor\n");
        return 1;
    }
    printf("PASS: a heap response body is released with the request\n");
    return 0;
#endif
}
