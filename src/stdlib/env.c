/*
 * env.c — Implementation of the std.env standard library module.
 *
 * malloc is permitted here: this is a stdlib boundary, not arena-managed
 * compiler code.
 *
 * POSIX APIs used: getenv, setenv.
 *
 * Story: 2.7.2  Branch: feature/stdlib-2.7-process-env
 */

#include "env.h"
#include <stdlib.h>
#include <string.h>
#include <errno.h>

/*
 * Validate that a key is non-empty and contains no '=' or NUL bytes.
 * setenv(3) has undefined behaviour with such keys.
 */
static int key_is_valid(const char *key)
{
    if (!key || *key == '\0') return 0;
    for (const char *p = key; *p; p++) {
        if (*p == '=') return 0;
    }
    return 1;
}

EnvGetResult env_get(const char *key)
{
    EnvGetResult r = {NULL, 0, ENV_ERR_NONE, NULL};

    if (!key_is_valid(key)) {
        r.is_err  = 1;
        r.err_kind = ENV_ERR_INVALID;
        r.err_msg  = "invalid environment variable key";
        return r;
    }

    const char *val = getenv(key);
    if (!val) {
        r.is_err  = 1;
        r.err_kind = ENV_ERR_NOT_FOUND;
        r.err_msg  = "environment variable not found";
        return r;
    }

    r.ok = val;
    return r;
}

const char *env_get_or(const char *key, const char *default_val)
{
    if (!key_is_valid(key)) return default_val;
    const char *val = getenv(key);
    return val ? val : default_val;
}

int env_set(const char *key, const char *val)
{
    if (!key_is_valid(key)) return 0;
    if (!val) return 0;
    /* setenv overwrites existing value (overwrite=1) */
    return setenv(key, val, 1) == 0 ? 1 : 0;
}
