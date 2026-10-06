/* SPDX-License-Identifier: MIT */
/*
 * os_glue.c — the std.os wrappers that report failure through the error slot.
 *
 * std.os is otherwise a thin POSIX bridge in os.c whose calls return the
 * syscall's own value (-1 and os.errno() on failure).  os.c is also linked
 * into tkc itself, which does not link tk_runtime.c, so a wrapper that raises
 * through @tk_current_error (runtime-abi.md §7) lives here, beside it, and is
 * linked only into programs that import std.os (src/stdlib_deps.c).
 *
 * Story: 121.70 (3)
 */

#include "os.h"
#include "tk_runtime.h"   /* tk_current_error, tk_err_box, tk_err_msg */
#include <errno.h>
#include <stdint.h>
#include <string.h>

/* stdlib/os.tki declares `oserr` as a record {msg:str; code:i64}: the box is
 * those two slots in that order (runtime-abi.md §7.3).  errno is restored
 * after the box is built, so os.errno() read in the $err arm agrees with
 * e.code. */
static int64_t os_raise(int code, int64_t written, size_t len) {
    int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
    box[0] = (int64_t)(intptr_t)tk_err_msg("os.write: %s: wrote %lld of %llu bytes",
                                           strerror(code), (long long)written,
                                           (unsigned long long)len);
    box[1] = (int64_t)code;
    tk_current_error = (int64_t)(intptr_t)box;
    errno = code;
    return written;
}

/* os.write(fd; data):i64!oserr — writes every byte of `data` to `fd`, or
 * fails.  Ok value: the byte count, which is the string's length (0 for "",
 * a success).  Err: the errno as e.code and a message with the count of
 * bytes that did reach the descriptor before the failure. */
int64_t tk_os_write_w(int64_t fd, int64_t data) {
    const char *s = (const char *)(intptr_t)data;
    if (!s) return os_raise(EINVAL, 0, 0);
    size_t len = strlen(s);
    OsWriteResult r = os_write_all((int)fd, s, len);
    if (r.err) return os_raise(r.err, r.written, len);
    tk_current_error = 0;
    return r.written;
}
