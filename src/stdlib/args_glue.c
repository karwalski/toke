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
