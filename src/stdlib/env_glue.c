/*
 * env_glue.c — i64-ABI wrappers for std.env module.
 *
 * Split from tk_web_glue.c so that --emit-deps can include only this file
 * when a program imports std.env.
 */

#include "env.h"
#include <stdint.h>

int64_t tk_env_get_or(int64_t key, int64_t def) {
    return (int64_t)(intptr_t)env_get_or(
        (const char *)(intptr_t)key,
        (const char *)(intptr_t)def);
}

int64_t tk_env_set_w(int64_t key, int64_t val) {
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
int64_t tk_env_get_w(int64_t key) { (void)key; return 0; }
