/* SPDX-License-Identifier: MIT */
/*
 * md_glue.c — i64-ABI wrappers for std.md module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.md.
 */

#include "md.h"
#include "capabilities.h"   /* 121.42 (5): md.renderfile is an fs.read sink */
#include <stdint.h>

int64_t tk_md_render_w(int64_t src) {
    if (!src) return 0;
    return (int64_t)(intptr_t)md_render((const char *)(intptr_t)src);
}

/*
 * Story 136.32 — md.renderfile(path) : $str!$mderr.
 *
 * md_render_file() has been in md.c and declared in md.h since 55.5.2, and
 * stdlib/md.tki exports md.renderfile; the wrapper never existed, so the
 * example on docs/stdlib/md.md failed at link. Error reporting follows the
 * tk_current_error protocol toml_glue.c uses.
 */
/* 127.101: tk_current_error is thread-local (runtime-abi.md §7, tk_runtime.h).
 * A plain-global declaration here links with no diagnostic and then SIGBUSes
 * on the first access, so the spelling must match the definition. */
#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 201112L
extern _Thread_local int64_t tk_current_error;
#else
extern __thread int64_t tk_current_error;
#endif

/* 121.42 (5): reads the file named by the program -- fs.read, like file.read. */
int64_t tk_md_renderfile_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) { tk_current_error = 1; return (int64_t)(intptr_t)""; }
    MdFileResult r = md_render_file((const char *)(intptr_t)path);
    tk_current_error = r.is_err ? 1 : 0;
    if (r.is_err) return (int64_t)(intptr_t)(r.err_msg ? r.err_msg : "");
    return (int64_t)(intptr_t)(r.ok ? r.ok : "");
}
