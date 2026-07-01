/*
 * fuzz_json.c — libFuzzer entry point for the std.json parser.
 *
 * Drives json_dec() over arbitrary bytes. json_dec() validates structure by
 * walking the input with the internal skip_string / skip_object / skip_array
 * routines, so this target directly exercises:
 *   - PAR-03 (json.c skip_string OOB read on trailing backslash)
 *   - PAR-04 (unbounded recursion in the value skippers → stack-overflow DoS)
 *
 * json_dec() does not allocate — on success r.ok.raw aliases the input, so
 * there is nothing to free.
 *
 * Build:  make fuzz-json   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/stdlib/json.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    /* Structure validation walks skip_string / skip_object / skip_array. */
    (void)json_dec(input);

    free(input);
    return 0;
}
