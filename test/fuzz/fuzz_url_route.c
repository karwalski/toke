/*
 * fuzz_url_route.c — libFuzzer entry point for URL routing pattern matching.
 *
 * Tests: has_traversal, match_route, and static file path construction
 * from router.c with arbitrary input paths.
 *
 * Build:  make fuzz-url-route   (FUZZ_ENGINE=libfuzzer|standalone|auto)
 * Story:  57.4.6
 */

#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>

/* Pull in router functions — we test the public API */
#include "../../src/stdlib/router.h"

/* 121.30: a private document root, so the 200 path (file read, ETag, the
 * headers it owns) is reachable and the run is the same on every host.  It
 * used to be /tmp, whose contents are whatever the machine has. */
static char g_root[256];
static void rm_root(void)
{
    char p[512];
    snprintf(p, sizeof p, "%s/index.html", g_root); unlink(p);
    snprintf(p, sizeof p, "%s/a/b.txt", g_root);    unlink(p);
    snprintf(p, sizeof p, "%s/a", g_root);          rmdir(p);
    rmdir(g_root);
}
static int make_root(void)
{
    const char *t = getenv("TMPDIR");
    snprintf(g_root, sizeof g_root, "%s/tkfuzz_route.XXXXXX", t && *t ? t : "/tmp");
    if (!mkdtemp(g_root)) return -1;
    char p[512];
    snprintf(p, sizeof p, "%s/index.html", g_root);
    FILE *f = fopen(p, "w"); if (f) { fputs("<p>i</p>", f); fclose(f); }
    snprintf(p, sizeof p, "%s/a", g_root); mkdir(p, 0700);
    snprintf(p, sizeof p, "%s/a/b.txt", g_root);
    f = fopen(p, "w"); if (f) { fputs("b", f); fclose(f); }
    atexit(rm_root);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (!g_root[0] && make_root() != 0) return 0;

    /* Limit input size to avoid OOM */
    if (size > 4096) return 0;

    /* Null-terminate the input */
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    /* Static file serving with an arbitrary relative path.  The traversal
     * checks and realpath validation must keep every answer inside g_root. */
    TkRouteResp resp = router_static_serve(g_root, input, NULL, NULL);

    /* Free what router_static_serve allocated.  Per router.h only a 200 or
     * 206 answer owns its body and header_values[0] (the ETag); every other
     * status carries a string literal.  (In fact every header value of a 200
     * is strdup'd and both arrays are malloc'd; all of it is released, or
     * libFuzzer's leak check reports it.)  121.30: this used to free the body
     * of EVERY answer, so the first 403/404 input aborted the harness in
     * free() -- it could not have run clean on any corpus, and no target
     * built it until now. */
    if (resp.status == 200 || resp.status == 206) {
        free((void *)resp.body);
        for (uint64_t i = 0; i < resp.nheaders; i++)
            free((void *)resp.header_values[i]);   /* each one strdup'd */
        free((void *)resp.header_names);          /* the arrays malloc'd */
        free((void *)resp.header_values);
    }

    free(input);
    return 0;
}
