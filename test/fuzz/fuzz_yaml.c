/*
 * fuzz_yaml.c — libFuzzer entry point for the std.yaml parser/converters.
 *
 * Drives the three untrusted-input entry points of yaml.c:
 *   - yaml_dec()        structure validation
 *   - yaml_from_json()  JSON → YAML  (PAR-01: snprintf-return accumulation
 *                        and int cap-pos underflow write past the heap buffer)
 *   - yaml_to_json()    YAML → JSON
 *
 * OWNERSHIP NOTE: yaml_from_json / yaml_to_json return EITHER a malloc'd
 * buffer OR a static string literal ("null", "{}", "\"\"") on the error
 * paths, so the returned pointer MUST NOT be freed. This target therefore
 * intentionally leaks the successful (malloc'd) results; run it with
 * ASAN_OPTIONS=detect_leaks=0 so LeakSanitizer does not mask the
 * heap-overflow crashes we are actually hunting. The inconsistent ownership
 * is itself tracked as a robustness issue (see 120.5 PAR-01).
 *
 * Build:  make fuzz-yaml   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/stdlib/yaml.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    (void)yaml_dec(input);
    (void)yaml_from_json(input);   /* result: literal-or-heap, do not free */
    (void)yaml_to_json(input);     /* result: literal-or-heap, do not free */

    free(input);
    return 0;
}
