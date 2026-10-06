/* SPDX-License-Identifier: MIT */
/*
 * env_glue.c — i64-ABI wrappers for std.env module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.env.
 */

#include "env.h"
#include "capabilities.h"   /* 124.4c: env.write capability gate */
#include "tk_runtime.h"     /* 127.241: tk_current_error + tk_err_box (127.109) */
#include <stdint.h>

int64_t tk_env_get_or(int64_t key, int64_t def) {
    return (int64_t)(intptr_t)env_get_or(
        (const char *)(intptr_t)key,
        (const char *)(intptr_t)def);
}

int64_t tk_env_set_w(int64_t key, int64_t val) {
    TK_REQUIRE(TK_CAP_ENV_WRITE);
    return (int64_t)env_set(
        (const char *)(intptr_t)key,
        (const char *)(intptr_t)val);
}

int64_t tk_env_getint_w(int64_t key, int64_t def) {
    return env_getint((const char *)(intptr_t)key, def);
}

int64_t tk_env_expand_w(int64_t tmpl) {
    return (int64_t)(intptr_t)env_expand((const char *)(intptr_t)tmpl);
}

int64_t tk_env_getor_w(int64_t key, int64_t def) { return tk_env_get_or(key, def); }
/* env.get(key) -> str!EnvErr.  127.241: a failure stores a {tag, msg} box
 * of EnvErr's sum layout — tag 0 $notfound, 1 $invalid, which is
 * EnvErrKind - 1 (env.h) — and a success clears the slot, so the $err arm
 * binds the variant; until then it returned a bare 0 and bound nil. */
int64_t tk_env_get_w(int64_t key) {
    EnvGetResult r = env_get((const char *)(intptr_t)key);
    if (r.is_err) {
        int64_t *box = (int64_t *)tk_err_box(2 * (int64_t)sizeof(int64_t));
        int inv = (r.err_kind == ENV_ERR_INVALID);
        box[0] = inv ? 1 : 0;
        box[1] = (int64_t)(intptr_t)(inv
            ? "env.get: invalid environment variable key"
            : "env.get: environment variable not set");
        tk_current_error = (int64_t)(intptr_t)box;
        return 0;
    }
    tk_current_error = 0;   /* 127.123: an ok return MUST clear the slot. */
    return (int64_t)(intptr_t)r.ok;
}
