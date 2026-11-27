/*
 * fuzz_yaml.c — libFuzzer entry point for the std.yaml parser/converters.
 *
 * Drives the three untrusted-input entry points of yaml.c:
 *   - yaml_dec()        structure validation
 *   - yaml_from_json()  JSON → YAML  (PAR-01: snprintf-return accumulation
 *                        and int cap-pos underflow write past the heap buffer)
 *   - yaml_to_json()    YAML → JSON
 *
 * OWNERSHIP NOTE: yaml_from_json / yaml_to_json return EITHER a heap
 * buffer OR a static string literal ("null", "{}", "\"\"") on the error
 * paths (120.5 PAR-01), so a result may be freed only once it is known not
 * to be a literal.  127.295: the results are sealed strings (never freed by
 * toke); this target builds with -DTK_STR_NO_HEAP, where a "sealed" string
 * is the malloc fallback, so here they can and must be freed.  Until
 * 127.555 this target left them allocated on purpose.
 *
 * 127.555: that leak is what failed the Security nightly every night
 * from 2026-11-24 (libFuzzer: out-of-memory, ~2060 MB of a 2048 MB
 * rss_limit_mb).  yaml_from_json amplifies by the block indent (up to
 * YAML_MAX_INDENT columns a line, so an 801-byte nested array is 66 KB of
 * YAML), and every result of every input stayed allocated, so RSS climbed
 * with the run and the input that crossed the limit was any input.  No
 * single input needs more than a few MB.  Each heap result is now freed;
 * a literal fallback is told apart by asking the sanitizer allocator whether
 * it owns the pointer (both fuzz engines build with -fsanitize=address).
 *
 * Build:  make fuzz-yaml   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <sanitizer/allocator_interface.h>

#include "../../src/stdlib/yaml.h"

/* 127.555: free a converter result unless it is one of yaml.c's literal
 * fallbacks ("", "{}", ...), which the allocator does not own. */
static void free_result(const char *r)
{
    if (r && __sanitizer_get_ownership(r)) free((void *)r);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    (void)yaml_dec(input);
    free_result(yaml_from_json(input));
    free_result(yaml_to_json(input));

    free(input);
    return 0;
}
