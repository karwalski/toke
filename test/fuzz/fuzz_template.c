/*
 * fuzz_template.c — libFuzzer entry point for the std.template engine.
 *
 * Drives tmpl_compile() over arbitrary template source and, on success,
 * renders it twice (plain + HTML-escaped) with an empty variable set. This
 * exercises the {{IDENT}} slot scanner and the render/escape substitution
 * paths (cross-ref 120.5 PAR-08 md_inline_cell recursion / PAR-09 template
 * engine recursion — the file-backed layout/partial paths are NOT driven
 * here because they require filesystem access).
 *
 * Build:  make fuzz-template   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/stdlib/template.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size > 65536) return 0;

    char *input = malloc(size + 1);
    if (!input) return 0;
    memcpy(input, data, size);
    input[size] = '\0';

    TkTmpl *t = tmpl_compile(input);
    if (t) {
        const char *r1 = tmpl_render(t, NULL, 0);
        if (r1) free((void *)r1);
        const char *r2 = tmpl_renderhtml(t, NULL, 0);
        if (r2) free((void *)r2);
        tmpl_free(t);
    }

    free(input);
    return 0;
}
