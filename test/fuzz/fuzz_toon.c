/*
 * fuzz_toon.c — libFuzzer entry point for the std.toon parser/converters.
 *
 * Drives the untrusted-input entry points of toon.c:
 *   - toon_dec()        schema validation
 *   - toon_from_json()  JSON → TOON  (PAR-02: single-object path uses a fixed
 *                        4096-byte buffer with no growth; PAR-07: single
 *                        doubling can still be too small)
 *   - toon_to_json()    TOON → JSON  (reaches toon_arr / parse_schema which
 *                        trusts the declared [count] — PAR-06)
 *
 * OWNERSHIP NOTE: toon_from_json / toon_to_json return EITHER a malloc'd
 * buffer OR a static string literal ("[]") on the error paths, so the
 * returned pointer MUST NOT be freed. This target intentionally leaks the
 * successful results; run with ASAN_OPTIONS=detect_leaks=0 (see 120.5
 * PAR-02/PAR-07).
 *
 * Build:  make fuzz-toon   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/stdlib/toon.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    (void)toon_dec(input);
    (void)toon_from_json(input);   /* result: literal-or-heap, do not free */
    (void)toon_to_json(input);     /* result: literal-or-heap, do not free */

    free(input);
    return 0;
}
