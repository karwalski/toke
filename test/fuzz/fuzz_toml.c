/*
 * fuzz_toml.c — libFuzzer entry point for the std.toml loader.
 *
 * Drives toml_load() over arbitrary bytes. toml_load() wraps the vendored
 * tomlc99 parser (src/stdlib/vendor/tomlc99/toml.c), so this target exercises
 * the whole TOML tokenizer/parser surface reachable from untrusted config
 * input. On success the returned table is released with toml_free_table().
 *
 * Build:  make fuzz-toml   (requires clang with -fsanitize=fuzzer)
 *         Links toml.o plus the vendored tomlc99 object; see fuzzing.md.
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/stdlib/toml.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    TomlResult r = toml_load(input);
    if (!r.is_err && r.ok)
        toml_free_table(r.ok);

    free(input);
    return 0;
}
