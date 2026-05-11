/*
 * md_glue.c — i64-ABI wrappers for std.md module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.md.
 */

#include "md.h"
#include <stdint.h>

int64_t tk_md_render_w(int64_t src) {
    if (!src) return 0;
    return (int64_t)(intptr_t)md_render((const char *)(intptr_t)src);
}
