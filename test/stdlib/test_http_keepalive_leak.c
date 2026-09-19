/*
 * test_http_keepalive_leak.c — regression test for story 127.65.
 *
 * handle_connection()'s keep-alive loop called parse_request() once per
 * request and never released the result: one strdup for the method, one for
 * the path, one for the body, two per header, and a flat
 * malloc(64 * sizeof(StrPair)) header block (1024 B on a 64-bit target)
 * regardless of how many headers actually arrived.  A long-lived toke HTTP
 * server therefore grew by roughly 1.1-1.3 KB per request, linearly, with no
 * plateau — including on a 404 path where no toke code runs at all.
 *
 * The test drives the real server loop through http_handle_fd() over a
 * loopback keep-alive connection and reads the process heap's in-use byte
 * count before and after.  Per-connection fixed costs are excluded by
 * warming up on an identical connection first, so what is left is per
 * request.  A leak of the old size shows up as > 1 KB/request; the fixed
 * code holds nothing across an iteration.
 *
 * Build and run: make test-stdlib-http-leak
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
#include "../../src/stdlib/http.h"

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

/* A request with a realistic header set — the header block is the bulk of
 * what the old code leaked. */
static const char REQ[] =
    "GET /no-such-route HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "User-Agent: tk-leak-probe/1\r\n"
    "Accept: */*\r\n"
    "Accept-Language: en\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

#define WARMUP_REQUESTS   64
#define MEASURE_REQUESTS  900   /* KEEPALIVE_MAX_REQUESTS is 1000 */

/* Old behaviour was ~1.1-1.3 KB per request.  Anything at or above this
 * threshold is the regression; the fixed loop sits near zero. */
#define MAX_BYTES_PER_REQUEST 64

static int g_listen_fd = -1;
static int g_port      = 0;

/* client_thread — send `n` keep-alive requests down one connection, read
 * each response in full, then close so the server loop returns. */
static void *client_thread(void *arg)
{
    long n = (long)(intptr_t)arg;
    struct sockaddr_in a;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    if (s < 0) return (void *)(intptr_t)-1;

    memset(&a, 0, sizeof a);
    a.sin_family = AF_INET;
    a.sin_port   = htons((uint16_t)g_port);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) < 0) {
        close(s); return (void *)(intptr_t)-1;
    }

    for (long i = 0; i < n; i++) {
        size_t off = 0;
        while (off < sizeof(REQ) - 1) {
            ssize_t w = write(s, REQ + off, sizeof(REQ) - 1 - off);
            if (w <= 0) { close(s); return (void *)(intptr_t)-1; }
            off += (size_t)w;
        }
        /* Read until the response body is complete. */
        char   buf[8192];
        size_t have = 0;
        long   want = -1;   /* total bytes: headers + Content-Length */
        for (;;) {
            ssize_t r = read(s, buf + have, sizeof(buf) - have);
            if (r <= 0) { close(s); return (void *)(intptr_t)-1; }
            have += (size_t)r;
            buf[have < sizeof(buf) ? have : sizeof(buf) - 1] = '\0';
            if (want < 0) {
                char *sep = strstr(buf, "\r\n\r\n");
                if (!sep) continue;
                long cl = 0;
                char *cp = strstr(buf, "Content-Length:");
                if (cp) cl = atol(cp + 15);
                want = (long)(sep - buf) + 4 + cl;
            }
            if ((long)have >= want) break;
        }
    }
    close(s);
    return (void *)(intptr_t)0;
}

/* serve_one — accept one connection and run the real keep-alive loop on it
 * while `n` requests are driven down it from a client thread. */
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
    alarm(120); /* safety net */

#if !HAVE_HEAP_STATS
    printf("SKIP: no heap-accounting API on this platform\n");
    return 0;
#else
    struct sockaddr_in a;
    socklen_t alen = sizeof a;

    g_listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (g_listen_fd < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(g_listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof opt);
    memset(&a, 0, sizeof a);
    a.sin_family      = AF_INET;
    a.sin_port        = 0;                      /* ephemeral */
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(g_listen_fd, (struct sockaddr *)&a, sizeof a) < 0) {
        perror("bind"); return 1;
    }
    if (listen(g_listen_fd, 4) < 0) { perror("listen"); return 1; }
    if (getsockname(g_listen_fd, (struct sockaddr *)&a, &alen) < 0) {
        perror("getsockname"); return 1;
    }
    g_port = ntohs(a.sin_port);

    /* Warm-up connection: settles every one-off allocation (log buffers,
     * rate-limit bucket, the per-connection read buffer's arena pages) so the
     * measured run sees per-request cost only. */
    if (serve_one(WARMUP_REQUESTS) != 0) {
        fprintf(stderr, "FAIL: warm-up connection did not complete\n");
        return 1;
    }

    size_t before = heap_in_use();
    if (serve_one(MEASURE_REQUESTS) != 0) {
        fprintf(stderr, "FAIL: measured connection did not complete\n");
        return 1;
    }
    size_t after = heap_in_use();

    long   delta     = (long)after - (long)before;
    double per_req   = (double)delta / (double)MEASURE_REQUESTS;

    printf("heap in use: before=%zu after=%zu delta=%ld over %d requests\n",
           before, after, delta, MEASURE_REQUESTS);
    printf("per request: %.1f bytes (threshold %d)\n",
           per_req, MAX_BYTES_PER_REQUEST);

    if (per_req >= (double)MAX_BYTES_PER_REQUEST) {
        fprintf(stderr,
                "FAIL: keep-alive loop retains %.1f bytes per request "
                "(story 127.65)\n", per_req);
        return 1;
    }
    printf("pass: keep-alive loop holds nothing across an iteration\n");
    printf("All tests passed.\n");
    close(g_listen_fd);
    return 0;
#endif
}
