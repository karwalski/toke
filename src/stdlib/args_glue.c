/*
 * args_glue.c — i64-ABI wrappers for std.args module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.args.
 */

#include "args.h"
#include <stdint.h>

int64_t tk_args_count_w(void) {
    return (int64_t)args_count();
}

int64_t tk_args_get_w(int64_t n) {
    StrArgsResult r = args_get((uint64_t)n);
    return r.is_err ? 0 : (int64_t)(intptr_t)r.ok;
}

/*
 * Story 136.32 — args.all().
 *
 * args_all() has been in args.c and declared in args.h since the module
 * shipped, stdlib/args.tki exports `args.all() : [str]`, and two examples on
 * docs/stdlib/args.md open with `let all = args.all();`. Only the wrapper was
 * missing, so both failed at link.
 *
 * Returns a real toke [str] (tk_array.h layout), not the bare StrArray struct:
 * a StrArray* would be read as an array handle whose [-1] length word is
 * whatever happens to precede it in memory.
 */
#include <stdlib.h>
#include "tk_array.h"

int64_t tk_args_all_w(void) {
    StrArray a = args_all();
    int64_t n = (int64_t)a.len;
    int64_t h = tk_arr_alloc(n, n);
    if (!h) return tk_arr_alloc(0, 0);
    int64_t *slots = (int64_t *)(intptr_t)h;
    for (int64_t i = 0; i < n; i++)
        slots[i] = a.data && a.data[i] ? (int64_t)(intptr_t)a.data[i]
                                       : (int64_t)(intptr_t)"";
    return h;
}
