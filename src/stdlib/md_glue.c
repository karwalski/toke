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
 * example on docs/stdlib/md.md failed at link.
 */
/* 113.B.7: the $err payload is an $mderr record {msg} (runtime-abi.md §7.3),
 * stored the way toml_glue.c's toml_raise stores a $tomlerr.  Until 113.B.7
 * the glue stored the payload-less flag 1, so `e.msg` in the $err arm read
 * address 1 and stopped the program with RT005; the message md_render_file
 * produced was returned as the VALUE, where nothing read it.  The message is
 * copied through tk_err_msg (the stdlib message ring, 127.272). */
#include "tk_runtime.h"   /* tk_current_error, tk_err_box, tk_err_msg */

static int64_t md_raise(const char *msg) {
    const char *m = tk_err_msg("%s", msg && msg[0] ? msg : "md error");
    int64_t *box = (int64_t *)tk_err_box((int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)m;
    tk_current_error = (int64_t)(intptr_t)box;
    return 0;
}

/* 121.42 (5): reads the file named by the program -- fs.read, like file.read. */
int64_t tk_md_renderfile_w(int64_t path) {
    TK_REQUIRE(TK_CAP_FS_READ);
    if (!path) return md_raise("md.renderfile: null path");
    MdFileResult r = md_render_file((const char *)(intptr_t)path);
    if (r.is_err) return md_raise(r.err_msg);
    tk_current_error = 0;
    return (int64_t)(intptr_t)(r.ok ? r.ok : "");
}
