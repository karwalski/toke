/*
 * path_glue.c — i64-ABI wrappers for std.path module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.path.
 */

#include "path.h"
#include <stdint.h>

int64_t tk_path_join_w(int64_t a, int64_t b) {
    if (!a || !b) return a ? a : b;
    return (int64_t)(intptr_t)path_join((const char *)(intptr_t)a, (const char *)(intptr_t)b);
}

int64_t tk_path_dir_w(int64_t p) {
    if (!p) return 0;
    return (int64_t)(intptr_t)path_dir((const char *)(intptr_t)p);
}

int64_t tk_path_ext_w(int64_t p) {
    if (!p) return 0;
    return (int64_t)(intptr_t)path_ext((const char *)(intptr_t)p);
}
