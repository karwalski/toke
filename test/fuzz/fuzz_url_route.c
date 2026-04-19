/*
 * fuzz_url_route.c — libFuzzer entry point for URL routing pattern matching.
 *
 * Tests: has_traversal, match_route, and static file path construction
 * from router.c with arbitrary input paths.
 *
 * Build:  make fuzz-url-route   (requires clang with -fsanitize=fuzzer)
 * Story:  57.4.6
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>

/* Pull in router functions — we test the public API */
#include "../../src/stdlib/router.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* Limit input size to avoid OOM */
    if (size > 4096) return 0;

    /* Null-terminate the input */
    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    /* Test static file serving with arbitrary paths.
     * Uses /tmp as a safe document root — no real files will be served.
     * The path traversal checks and realpath validation should prevent
     * any access outside /tmp. */
    TkRouteResp resp = router_static_serve("/tmp", input, NULL, NULL);

    /* Free any heap-allocated response data */
    if (resp.body) free((void *)resp.body);
    if (resp.nheaders > 0 && resp.header_values) {
        for (uint64_t i = 0; i < resp.nheaders; i++) {
            if (resp.header_values[i]) free((void *)resp.header_values[i]);
        }
    }

    free(input);
    return 0;
}
