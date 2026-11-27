/*
 * fuzz_toon.c — libFuzzer entry point for the std.toon parser/converters.
 *
 * Drives the untrusted-input entry points of toon.c:
 *   - toon_dec()        schema validation
 *   - toon_from_json()  JSON → TOON  (PAR-02: single-object path uses a fixed
 *                        4096-byte buffer with no growth; PAR-07: single
 *                        doubling can still be too small)
 *   - toon_to_json()    TOON → JSON  (parse_schema)
 *   - toon_arr() and the scalar accessors on field "id" and on the schema's
 *                        first field (121.16: toon_arr trusted the declared
 *                        [count] -- PAR-06 -- and returned uninitialised
 *                        elements; each returned element is read here, so a
 *                        length that overstates what was filled faults)
 *
 * OWNERSHIP NOTE: toon_from_json / toon_to_json return EITHER a heap
 * buffer OR a static string literal ("[]") on the error paths (120.5
 * PAR-02/PAR-07), so a result may be freed only once it is known not to be
 * a literal.  toon_dec owns nothing: its Toon points at the input and its
 * error messages are literals.
 *
 * 127.556: until now this target left every converter result allocated, so
 * a libFuzzer run's RSS grew with the number of inputs until the Security
 * nightly hit its 2048 MB rss_limit_mb (crash-toon-oom-a09c1372, 1,462
 * bytes, ~10 MB replayed alone) -- the shape 127.555 fixed in fuzz_yaml.c.
 * Each heap result is now freed; a literal is told apart by asking the
 * sanitizer allocator whether it owns the pointer (both fuzz engines build
 * with -fsanitize=address).  fuzz-smoke replays this target with
 * LeakSanitizer on under Linux (FUZZ_LEAK_TARGETS).
 *
 * Build:  make fuzz-toon   (requires clang with -fsanitize=fuzzer)
 * Story:  120.22; toon_arr and the accessors added by 121.16
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "../../src/stdlib/tk_c99_string.h" /* 127.439: C99 strchr & co. on glibc */

#include <sanitizer/allocator_interface.h>

#include "../../src/stdlib/toon.h"

/* 127.556: free a converter result unless it is one of toon.c's literal
 * fallbacks, which the allocator does not own. */
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

    (void)toon_dec(input);         /* owns nothing (see the note above) */
    free_result(toon_from_json(input));
    free_result(toon_to_json(input));

    ToonResult d = toon_dec(input);
    if (!d.is_err) {
        /* the first field name, if the schema has one */
        char first[64] = "id";
        const char *b = strchr(input, '{');
        if (b) {
            size_t n = strcspn(b + 1, ",}");
            if (n > 0 && n < sizeof first) { memcpy(first, b + 1, n); first[n] = '\0'; }
        }
        const char *keys[2] = { "id", first };
        for (int k = 0; k < 2; k++) {
            ToonArrayResult a = toon_arr(d.ok, keys[k]);
            if (!a.is_err) {
                size_t total = 0;
                for (uint64_t i = 0; i < a.ok.len; i++) {
                    total += strlen(a.ok.data[i].raw);   /* every element must be real */
                    free((char *)a.ok.data[i].raw);
                }
                free(a.ok.data);
                (void)total;
            }
            StrToonResult s = toon_str(d.ok, keys[k]);
            if (!s.is_err) free((char *)s.ok);
            (void)toon_i64(d.ok, keys[k]);
            (void)toon_f64(d.ok, keys[k]);
            (void)toon_bool(d.ok, keys[k]);
        }
    }

    free(input);
    return 0;
}
