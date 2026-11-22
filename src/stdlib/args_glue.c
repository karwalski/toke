/* SPDX-License-Identifier: MIT */
/*
 * args_glue.c — i64-ABI wrappers for std.args module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.args.
 */

#include "args.h"
#include "tk_runtime.h"   /* 127.241: tk_current_error + tk_err_box (127.109) */
#include <stdint.h>

int64_t tk_args_count_w(void) {
    return (int64_t)args_count();
}

/* args.get(n) -> str!ArgsErr.  127.241: a failure stores an ArgsErr record
 * {msg} (runtime-abi.md §7.3) and a success clears the slot, so the $err arm
 * binds `e.msg`; until then it returned a bare 0 and `e.msg` was RT005. */
int64_t tk_args_get_w(int64_t n) {
    StrArgsResult r = args_get((uint64_t)n);
    if (r.is_err) {
        int64_t *box = (int64_t *)tk_err_box((int64_t)sizeof(int64_t));
        /* 113.B.8: the core's own reason, not a constant standing in for it,
         * so a new failure args_get reports reaches `e.msg` as it says it.
         * 127.470: but formatting it costs a vsnprintf on EVERY miss (the
         * canonical `mt args.get(1){..}` default form ran ~11x slower than
         * the count check).  The one reason args_get reports today is
         * ARGS_ERR_OOB, recognised by address (a strcmp per miss cost as much
         * again), so its message is a static literal too (same text, and it
         * outlives the ring); any other reason is still formatted. */
        const char *m;
        if (r.err_msg == ARGS_ERR_OOB)
            m = "args.get: index out of bounds";
        else
            m = tk_err_msg("args.get: %s", r.err_msg ? r.err_msg : "failed");
        box[0] = (int64_t)(intptr_t)m;
        tk_current_error = (int64_t)(intptr_t)box;
        return 0;
    }
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return (int64_t)(intptr_t)r.ok;
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
