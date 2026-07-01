/*
 * fuzz_multipart.c — libFuzzer entry point for multipart/form-data parsing.
 *
 * Drives http_multipart_boundary() and http_multipart_parse() — the parser
 * that consumes untrusted request bodies on file-upload endpoints. To get
 * good coverage the fuzzer splits the input at the first newline: the first
 * line is used as the boundary token and the remainder is parsed as the body
 * (a fixed fallback boundary is used when there is no newline). The
 * boundary-extraction helper is also fuzzed directly with the raw input.
 *
 * Cross-ref 120.6 (http-core body handling) — the multipart split logic and
 * mp_find scanning run on attacker bytes with no auth required.
 *
 * NOTE: http_multipart_parse lives in http.c. The OpenSSL/TLS code paths in
 * http.c are macro-guarded (compiled out unless TK_HAVE_OPENSSL is defined),
 * so this target needs only http.c + encoding.c + str.c + log.c and -lz — no
 * OpenSSL. See fuzzing.md for the exact link line.
 *
 * Build:  make fuzz-multipart   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/stdlib/http.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    /* Exercise boundary extraction directly on the raw input. */
    const char *extracted = http_multipart_boundary(input);
    if (extracted) free((void *)extracted);

    /* Split: first line = boundary, remainder = body. */
    char *nl = memchr(input, '\n', size);
    const char *boundary;
    const char *body;
    size_t body_len;
    char *bcopy = NULL;

    if (nl && nl != input) {
        size_t blen = (size_t)(nl - input);
        bcopy = malloc(blen + 1);
        if (!bcopy) { free(input); return 0; }
        memcpy(bcopy, input, blen);
        bcopy[blen] = '\0';
        boundary = bcopy;
        body = nl + 1;
        body_len = size - blen - 1;
    } else {
        boundary = "----fuzzBoundary";
        body = input;
        body_len = size;
    }

    TkMultipartResult r = http_multipart_parse(boundary, body, body_len);
    http_multipart_free(r);

    free(bcopy);
    free(input);
    return 0;
}
