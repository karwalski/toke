#ifndef TK_STDLIB_ENV_H
#define TK_STDLIB_ENV_H

/*
 * env.h — C interface for the std.env standard library module.
 *
 * Type mappings:
 *   Str        = const char *   (null-terminated UTF-8)
 *   bool       = int            (0 = false, 1 = true)
 *   Str!EnvErr = EnvGetResult
 *
 * Story: 2.7.2  Branch: feature/stdlib-2.7-process-env
 */

#include <stdint.h>

/* EnvErr variant enum */
typedef enum {
    ENV_ERR_NONE     = 0,
    ENV_ERR_NOT_FOUND = 1,  /* key is not set in the environment */
    ENV_ERR_INVALID   = 2   /* key or value contains an invalid character (e.g. '=', NUL) */
} EnvErrKind;

/* Result type for env.get */
typedef struct { const char *ok; int is_err; EnvErrKind err_kind; const char *err_msg; } EnvGetResult;

/*
 * env_get — look up an environment variable.
 * Returns EnvGetResult.ok (pointer into the live environ, do not free) on success,
 * or is_err=1 / err_kind=ENV_ERR_NOT_FOUND if the key is absent.
 */
EnvGetResult env_get(const char *key);

/*
 * env_get_or — look up a key; return default_val if the key is absent.
 * Never returns an error — missing key yields the default.
 */
const char  *env_get_or(const char *key, const char *default_val);

/*
 * env_set — set key=val in the process environment using setenv(3).
 * Returns 1 on success, 0 on failure (invalid key/value or OS error).
 */
int          env_set(const char *key, const char *val);

#endif /* TK_STDLIB_ENV_H */
