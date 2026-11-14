/*
 * test_http_route_param_leak.c — regression test for story 127.72.
 *
 * match_pattern() malloc()s a key and a value for every `:param` segment it
 * binds and writes them into the caller's StrPair block.  Nothing ever freed
 * them.  Three separate call sites leaked:
 *
 *   1. The first routing pass (http.c:1048) calls match_pattern for EVERY
 *      route.  On a route whose path matches but whose method does not, it
 *      then does `pc = 0` — dropping the pointers on the floor.  So the cost
 *      is not a constant per request: it scales with the number of `:param`
 *      routes whose PATH matches.
 *   2. The same pass breaks out on the selected route without freeing the
 *      pairs it just bound.
 *   3. The winner is re-matched at http.c:1064, which allocates a fresh pair
 *      set that the handler uses and nobody releases afterwards.
 *
 * A route with no `:param` segment allocates nothing, which is why a plain
 * route showed no growth and this went unnoticed.
 *
 * The test drives the real server loop through http_handle_fd() over a
 * loopback keep-alive connection, the same way test_http_keepalive_leak.c
 * does, and reads the process heap's in-use byte count before and after.
 *
 * TWO SCENARIOS, because one would not have caught the shape of the defect:
 *
 *   A. A single GET `:param` route.  Covers call sites 2 and 3.
 *   B. The same request against a table that also holds several `:param`
 *      routes registered for a DIFFERENT method.  Their paths match, so the
 *      first pass binds and drops a pair set for each — call site 1.  B must
 *      not cost meaningfully more than A, or the `pc = 0` path is still
 *      leaking.
 *
 * Build and run: make test-stdlib-http-route-param-leak
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */
#include <stdint.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "../../src/stdlib/http.h"

#include "heap_in_use.h"   /* 122.14: ASan-aware heap counter */

/* Four bound parameters, so a leak is four key/value pairs per match rather
 * than one — well clear of allocator noise.
 *
 * Two independent patterns because the first routing pass BREAKS on the first
 * route whose method also matches.  Scenario B's non-GET routes therefore
 * have to be registered ahead of its GET route, and that ordering cannot be
 * shared with scenario A. */
#define A_PATTERN "/a1/:a/:b/:c/:d"
#define B_PATTERN "/b1/:a/:b/:c/:d"

static const char REQ_A[] =
    "GET /a1/alpha/bravo/charlie/delta HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

static const char REQ_B[] =
    "GET /b1/alpha/bravo/charlie/delta HTTP/1.1\r\n"
    "Host: 127.0.0.1\r\n"
    "Connection: keep-alive\r\n"
    "\r\n";

#define WARMUP_REQUESTS   64
#define MEASURE_REQUESTS  900   /* KEEPALIVE_MAX_REQUESTS is 1000 */
/*
 * Several measured connections, not one.  What is left after the fix is not
 * per-request at all: it is the allocator growing its zone in 16 KB steps and
 * a few KB of per-connection cost, both of which are constant in the request
 * count.  Over a single 900-request connection that fixed cost reads as up to
 * ~36 B/request and would sit uncomfortably close to the threshold; over
 * eight it amortises to under 10, while a real per-request leak — 266 B
 * before the fix — is untouched by the averaging.  Verified directly: at 12
 * connections the delta for a scenario was 49152 bytes over 10800 requests
 * (3 x 16 KB), i.e. flat, where before the fix it was linear.
 */
#define MEASURE_CONNS     8

/*
 * Measured on this tree BEFORE the fix, macOS/arm64: see the printed numbers
 * in the commit for 127.72.  Scenario A came to 251.4 B/request — exactly the
 * two leaked pair-sets the code predicts (8 allocations of <= 16 B each, twice
 * over) — and scenario B to roughly three times that, because each additional
 * path-matching route adds one more dropped set.  After the fix both are 0.0.
 *
 * 32 leaves room for allocator bookkeeping noise while being far below even
 * scenario A's leak, and is the same order as the 64 that S001 uses.
 */
#define MAX_BYTES_PER_REQUEST 32

static int g_listen_fd = -1;
static int g_port      = 0;

static Res ok_handler(Req req)
{
    (void)req;
    return http_Res_ok("ok");
}

/* The request the current scenario drives; set before each measure(). */
static const char *g_req     = REQ_A;
static size_t      g_req_len = sizeof(REQ_A) - 1;

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
        while (off < g_req_len) {
            ssize_t w = write(s, g_req + off, g_req_len - off);
            if (w <= 0) { close(s); return (void *)(intptr_t)-1; }
            off += (size_t)w;
        }
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

/* measure — warm up, then run MEASURE_REQUESTS and report bytes/request.
 * Returns -1 if a connection did not complete. */
static double measure(const char *label)
{
    if (serve_one(WARMUP_REQUESTS) != 0) {
        fprintf(stderr, "FAIL: %s: warm-up connection did not complete\n", label);
        return -1.0;
    }
    printf("heap counter: %s\n", HEAP_STATS_SOURCE);
    size_t before = heap_in_use();
    for (int k = 0; k < MEASURE_CONNS; k++) {
        if (serve_one(MEASURE_REQUESTS) != 0) {
            fprintf(stderr, "FAIL: %s: measured connection %d did not complete\n",
                    label, k);
            return -1.0;
        }
    }
    size_t after = heap_in_use();
    long   delta = (long)after - (long)before;
    int    reqs  = MEASURE_REQUESTS * MEASURE_CONNS;
    double per   = (double)delta / (double)reqs;
    printf("%s: heap in use before=%zu after=%zu delta=%ld over %d requests\n",
           label, before, after, delta, reqs);
    printf("%s: %.1f bytes per request (threshold %d)\n",
           label, per, MAX_BYTES_PER_REQUEST);
    return per;
}

int main(void)
{
    alarm(180); /* safety net */

#if !HAVE_HEAP_STATS
    printf("SKIP: no heap-accounting API on this platform\n");
    return 0;
#else
    struct sockaddr_in a;
    socklen_t alen = sizeof a;
    int failed = 0;

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
    if (listen(g_listen_fd, 8) < 0) { perror("listen"); return 1; }
    if (getsockname(g_listen_fd, (struct sockaddr *)&a, &alen) < 0) {
        perror("getsockname"); return 1;
    }
    g_port = ntohs(a.sin_port);

    /* Registration order is load-bearing.  The first routing pass breaks on
     * the first route whose method matches too, so B's four non-GET routes
     * must sit AHEAD of its GET route to be reached at all — that is the
     * `pc = 0` path, and registering them after the winner would silently
     * test nothing. */
    http_GET(A_PATTERN, ok_handler);        /* scenario A: winner at once  */
    http_POST(B_PATTERN, ok_handler);       /* scenario B: path matches,   */
    http_PUT(B_PATTERN, ok_handler);        /*   method does not — bound   */
    http_DELETE(B_PATTERN, ok_handler);     /*   and then dropped, four    */
    http_PATCH(B_PATTERN, ok_handler);      /*   times over                */
    http_GET(B_PATTERN, ok_handler);        /* scenario B: the winner      */

    /* ── Scenario A: the selected route only (call sites 2 and 3) ── */
    g_req = REQ_A; g_req_len = sizeof(REQ_A) - 1;
    double per_a = measure("A(one :param route)");
    if (per_a < 0.0) return 1;
    if (per_a >= (double)MAX_BYTES_PER_REQUEST) {
        fprintf(stderr, "FAIL: a matched :param route retains %.1f bytes "
                        "per request (story 127.72)\n", per_a);
        failed = 1;
    } else {
        printf("pass: A holds nothing across a request\n");
    }

    /* ── Scenario B: four routes whose PATH matches but whose METHOD does
     *    not, ahead of the winner.  The first pass binds and then drops a
     *    pair set for each, so a fix that only freed the selected route's
     *    pairs still grows here — linearly in the number of such routes. ── */
    g_req = REQ_B; g_req_len = sizeof(REQ_B) - 1;
    double per_b = measure("B(4 path-matching non-GET routes first)");
    if (per_b < 0.0) return 1;
    if (per_b >= (double)MAX_BYTES_PER_REQUEST) {
        fprintf(stderr, "FAIL: the non-selected routes of the first pass "
                        "retain %.1f bytes per request (story 127.72)\n", per_b);
        failed = 1;
    } else {
        printf("pass: B holds nothing across a request\n");
    }

    close(g_listen_fd);
    if (failed) return 1;
    printf("All tests passed.\n");
    return 0;
#endif
}
